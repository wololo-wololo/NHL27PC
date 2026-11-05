#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/gpuCheckpoints.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "common/threadSampler.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace Libs::Graphics {

std::atomic<uint32_t> g_submit_probe {0};

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit(uint64_t tick) {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick, tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = tick;
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = tick;
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics,
                                   bool allow_async)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	// KYTY_ASYNC_RECORD=1: record Vulkan commands on a worker thread (see CommandRecorder).
	static const bool async = [] {
		const char* value = std::getenv("KYTY_ASYNC_RECORD");
		return value != nullptr && value[0] == '1';
	}();
	if (allow_async && async) {
		m_recorder = std::make_unique<CommandRecorder>(
		    [this](uint64_t tick) {
			    const auto buffer = m_command_pool.Commit(tick);
			    vk::CommandBufferBeginInfo begin_info {};
			    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
			    EXIT_NOT_IMPLEMENTED(buffer.begin(&begin_info) != vk::Result::eSuccess);
			    return buffer;
		    },
		    [this] { return CurrentTick(); });
		m_command.m_recorder = m_recorder.get();
		std::printf("GPU: asynchronous command recording enabled\n");
	}
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
	if (m_timestamps) {
		m_graphics.device.destroyQueryPool(m_timestamps, nullptr);
	}
}

constexpr uint32_t GPU_TIMING_SLOTS = 4096;

void CommandScheduler::BeginGpuTiming() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_TIMING");
		return value != nullptr && value[0] == '1';
	}();
	if (!enabled) {
		return;
	}
	if (!m_timestamps) {
		const vk::QueryPoolCreateInfo info {.queryType  = vk::QueryType::eTimestamp,
		                                    .queryCount = GPU_TIMING_SLOTS * 2u};
		RequireVulkanSuccess(m_graphics.device.createQueryPool(&info, nullptr, &m_timestamps),
		                     "vkCreateQueryPool");
	}
	m_timestamp_query = (m_timestamp_slot++ % GPU_TIMING_SLOTS) * 2u;
	m_command.RawHandle().resetQueryPool(m_timestamps, m_timestamp_query, 2u);
	m_command.RawHandle().writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_timestamps,
	                                     m_timestamp_query);
}

void CommandScheduler::EndGpuTiming() {
	if (m_timestamp_query == UINT32_MAX) {
		return;
	}
	const auto query   = m_timestamp_query;
	m_timestamp_query  = UINT32_MAX;
	m_command.RawHandle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_timestamps,
	                                     query + 1u);
	// Submissions also happen without a bound context, which DeferOperation requires.
	std::lock_guard lock(m_operation_mutex);
	if (m_operation_state != OperationState::Open) {
		return;
	}
	m_pending_operations.push({[this, query] {
		std::array<uint64_t, 2> times {};
		if (m_graphics.device.getQueryPoolResults(m_timestamps, query, 2, sizeof(times),
		                                          times.data(), sizeof(uint64_t),
		                                          vk::QueryResultFlagBits::e64) !=
		    vk::Result::eSuccess) {
			return;
		}
		const double period = m_graphics.physical_device_properties.limits.timestampPeriod;
		using Common::DebugCounters::Counter;
		Common::DebugCounters::Add(Counter::GpuTimedSubmits);
		if (times[1] > times[0]) {
			Common::DebugCounters::Add(
			    Counter::GpuBusyNs, static_cast<uint64_t>(static_cast<double>(times[1] - times[0]) * period));
		}
		if (m_last_gpu_end != 0 && times[0] > m_last_gpu_end) {
			Common::DebugCounters::Add(
			    Counter::GpuGapNs, static_cast<uint64_t>(static_cast<double>(times[0] - m_last_gpu_end) * period));
		}
		m_last_gpu_end = std::max(m_last_gpu_end, times[1]);
	}, CurrentTick()});
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	const auto tick = Submit();
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick);
		BeginNext();
	} else {
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

void CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		queue.push({std::move(operation), CurrentTick()});
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	EXIT_IF(m_command.IsInvalid());
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	if (m_recorder != nullptr) {
		m_command.m_open  = true;
		m_command.m_epoch = CurrentTick();
		m_command.Begin();
		BeginGpuTiming();
		GpuProfileBeginCommandBuffer(*this, m_command.RawHandle());
		m_dynamic_state.Invalidate();
		return m_command;
	}
	m_command.m_buffer = m_command_pool.Commit(CurrentTick());
	m_command.Begin();
	BeginGpuTiming();
	GpuProfileBeginCommandBuffer(*this, m_command.RawHandle());
	m_dynamic_state.Invalidate();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	if (g_submit_probe.load(std::memory_order_relaxed) != 0) {
		static std::atomic<uint32_t> reported {0};
		const auto                   n = reported.fetch_add(1, std::memory_order_relaxed);
		if (n < 40) {
			const bool gpu_thread = GuestGpu::IsGpuThread();
			std::printf("submit inside dispatch window (probe %u): gpu_thread=%d debug_op=%u "
			            "args=0x%x,0x%x,0x%x\n",
			            g_submit_probe.load(std::memory_order_relaxed), gpu_thread ? 1 : 0,
			            m_command.m_debug_op, m_command.m_debug_arg0, m_command.m_debug_arg1,
			            m_command.m_debug_arg2);
			static std::atomic<uint32_t> stacks_gpu {0};
			static std::atomic<uint32_t> stacks_other {0};
			if ((gpu_thread ? stacks_gpu : stacks_other).fetch_add(1) < (gpu_thread ? 1u : 6u)) {
				Common::ThreadSampler::PrintCurrentStack("  submit stack");
			}
		}
	}
	m_command.RunRecordHook();
	m_command.FlushComputeBarrier();
	EndGpuTiming();
	if (m_recorder != nullptr) {
		GpuProfileEndCommandBuffer(m_command.RawHandle(), CurrentTick());
		m_command.End();
		Common::DebugCounters::Add(Common::DebugCounters::Counter::QueueSubmits);
		// Ticks are assigned in recording order and the worker submits in the same order.
		const auto tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);
		struct Debug {
			uint32_t op;
			uint64_t submit_id;
			uint32_t arg0, arg1, arg2, arg3;
			uint64_t arg4;
		} debug {m_command.m_debug_op,   m_command.m_debug_submit_id, m_command.m_debug_arg0,
		         m_command.m_debug_arg1, m_command.m_debug_arg2,      m_command.m_debug_arg3,
		         m_command.m_debug_arg4};
		m_recorder->Submit([this, submit, tick, debug](vk::CommandBuffer buffer) {
			EXIT_NOT_IMPLEMENTED(buffer.end() != vk::Result::eSuccess);
			auto& graphics = m_graphics;
			vk::Result result;
			{
				Common::LockGuard lock(graphics.queue_mutex);
				vk::TimelineSemaphoreSubmitInfo timeline_info {};
				timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
				timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
				timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
				timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();
				vk::SubmitInfo submit_info {};
				submit_info.pNext                = &timeline_info;
				submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
				submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
				submit_info.pWaitDstStageMask    = submit.wait_stages.data();
				submit_info.commandBufferCount   = 1;
				submit_info.pCommandBuffers      = &buffer;
				submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
				submit_info.pSignalSemaphores    = submit.signal_semaphores.data();
				result = graphics.queue.submit(1, &submit_info, nullptr);
			}
			if (result != vk::Result::eSuccess) {
				ReportGpuCheckpoints(graphics);
				ReportVulkanFatal("vkQueueSubmit", result, tick, debug.op, debug.submit_id,
				                  debug.arg0, debug.arg1, debug.arg2, debug.arg3, debug.arg4);
			}
			EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
		});
		m_command.m_open = false;
		return tick;
	}
	GpuProfileEndCommandBuffer(m_command.RawHandle(), CurrentTick());
	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	Common::DebugCounters::ScopedTimer submit_timer(
	    Common::DebugCounters::Counter::QueueSubmits,
	    Common::DebugCounters::Counter::QueueSubmitNs);
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}
	if (result != vk::Result::eSuccess) {
		ReportGpuCheckpoints(graphics);
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
