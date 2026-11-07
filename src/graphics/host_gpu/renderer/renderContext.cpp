#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics, const std::filesystem::path& shader_store)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics, true),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics, &m_image_heap, shader_store), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_image_heap(*this, m_page_manager), m_occlusion_queries(graphics, m_command_scheduler) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	using Clock = std::chrono::steady_clock;
	const bool profile = Common::DebugCounters::Enabled();
	auto stamp = profile ? Clock::now() : Clock::time_point {};
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	auto record = [&](Common::DebugCounters::Counter counter) {
		if (!profile) {
			return;
		}
		const auto now = Clock::now();
		Common::DebugCounters::Add(
		    counter, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                       now - stamp)
		                                       .count()));
		stamp = now;
	};
	record(Common::DebugCounters::Counter::GpuFaultMappedNs);
	if (access == PageFaultAccess::Write) {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::GpuFaultWrites);
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		record(Common::DebugCounters::Counter::GpuFaultBufferNs);
		m_texture_cache.PreserveGpuImagesForCpuWrite(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		record(Common::DebugCounters::Counter::GpuFaultTextureNs);
		m_image_heap.InvalidateMemory(fault_vaddr, fault_size);
		record(Common::DebugCounters::Counter::GpuFaultImageHeapNs);
	} else {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::GpuFaultReads);
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
		record(Common::DebugCounters::Counter::GpuFaultBufferNs);
	}
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.PreserveGpuImagesForCpuWrite(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	m_image_heap.InvalidateMemory(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size, bool mirror) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
	if (mirror && (BufferCache::MirrorEnabled() || BufferCache::HostMirrorEnabled())) {
		m_buffer_cache.QueueMirror(vaddr, size);
	}
	{
		// GPU-visible guest memory, for sizing a full mirror: print each 256 MiB step.
		static const bool trace = std::getenv("KYTY_GPU_MIRROR_TRACE") != nullptr;
		static uint64_t total = 0, printed = 0;
		total += size;
		if (trace && total / (256u << 20) != printed) {
			printed = total / (256u << 20);
			std::printf("GPU-visible guest memory mapped: %.2f GiB (last 0x%llx +0x%llx)\n",
			            static_cast<double>(total) / (1u << 30), static_cast<unsigned long long>(vaddr),
			            static_cast<unsigned long long>(size));
		}
	}
	m_buffer_cache.QueueBdaSyncCpu(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Check cache ownership on the GPU thread. Guest-memory callbacks can still
		// access a range with no cached data, so they must finish before it is unmapped.
		const bool priority =
		    m_command_scheduler.Active() && m_command_scheduler.HasPendingPriorityOperations();
		if (m_command_scheduler.Active() &&
		    (m_buffer_cache.IsRegionRegistered(vaddr, size) ||
		     m_texture_cache.IsRegionRegistered(vaddr, size) ||
		     m_command_scheduler.HasPendingPriorityOperations())) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		if (BufferCache::MirrorEnabled() || BufferCache::HostMirrorEnabled()) {
			m_buffer_cache.UnmapMirror(vaddr, size);
		}
		m_texture_cache.UnmapMemory(vaddr, size);
		m_image_heap.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBdaForSubmission() {
	if (!BufferCache::MirrorEnabled() && !BufferCache::BdaSliceSyncEnabled()) {
		return;
	}
	m_bda_submission_sync = true;
	PrepareBda();
	m_bda_submission_sync = false;
}

void RenderContext::PrepareBda() {
	if (BufferCache::MirrorEnabled() && !m_bda_submission_sync) {
		m_fault_process_pending = true;
		return;
	}
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	m_buffer_cache.ApplyMirror();
	std::shared_lock lock(m_mapped_ranges_mutex);
	// Per DMA draw only buffers registered since the last sync; CPU writes are synced at the start
	// of each submission slice (BufferCache::QueueBdaSyncCpu), unless slice sync is off.
	const bool all = m_bda_submission_sync || BufferCache::MirrorEnabled() ||
	                 !BufferCache::BdaSliceSyncEnabled();
	m_buffer_cache.TakeBdaSync(all).ForEach([this](uint64_t begin, uint64_t end) {
		m_mapped_ranges.ForEachInRange(begin, end - begin, [this](uint64_t start, uint64_t finish) {
			static const bool trace = std::getenv("KYTY_GPU_MIRROR_TRACE") != nullptr;
			if (trace && finish - start >= (1u << 20)) {
				std::printf("BDA sync: 0x%llx +0x%llx\n", static_cast<unsigned long long>(start),
				            static_cast<unsigned long long>(finish - start));
				std::fflush(stdout);
			}
			m_buffer_cache.SynchronizeBuffersInRange(start, finish - start);
			if (trace && finish - start >= (1u << 20)) {
				std::printf("BDA sync done\n");
				std::fflush(stdout);
			}
		});
	});
	m_fault_process_pending = true;
}

void RenderContext::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_image_heap.RunGarbageCollector();
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	Common::DebugCounters::Add(Common::DebugCounters::Counter::InterruptsDelivered);
	if (registrations.empty()) {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::InterruptsUnregistered);
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
			std::printf("GPU interrupt event=%d context=0x%x has no registered event queue\n",
			            event_id, context_id);
		}
	}
	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
