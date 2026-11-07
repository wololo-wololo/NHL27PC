#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/gpuCheckpoints.h"
#include "common/threadSampler.h"

#include "graphics/guest_gpu/graphicsRun.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		ReportGpuCheckpoints(m_graphics);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	// KYTY_WAIT_STACKS=<seconds>: from that long after the first blocking wait, print the caller
	// stack of every 16th blocking wait on the GPU thread (first 60).
	static const char* wait_env    = std::getenv("KYTY_WAIT_STACKS");
	if (wait_env != nullptr && GuestGpu::IsGpuThread()) {
		static const auto start = std::chrono::steady_clock::now() +
		                          std::chrono::seconds(std::atoi(wait_env));
		static std::atomic<uint32_t> waits {0};
		if (std::chrono::steady_clock::now() >= start) {
			const auto n = waits.fetch_add(1, std::memory_order_relaxed);
			if (n % 16u == 0u && n / 16u < 60u) {
				Common::ThreadSampler::PrintCurrentStack("  gpu wait stack");
			}
		}
	}
	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::GpuWaits,
	                                         Common::DebugCounters::Counter::GpuWaitNs);
	std::optional<Common::DebugCounters::ScopedTimer> gpu_thread_timer;
	if (Common::DebugCounters::Enabled() && GuestGpu::IsGpuThread()) {
		gpu_thread_timer.emplace(Common::DebugCounters::Counter::GpuThreadWaits,
		                         Common::DebugCounters::Counter::GpuThreadWaitNs);
	}
	// KYTY_WAIT_STACKS_MS=<ms>: print the caller stack of GPU-thread waits longer than that
	// (first 80), to find what the GPU thread waits for.
	static const int long_wait_ms = [] {
		const char* value = std::getenv("KYTY_WAIT_STACKS_MS");
		return value != nullptr ? std::atoi(value) : 0;
	}();
	const auto wait_begin = std::chrono::steady_clock::now();
	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (long_wait_ms > 0 && GuestGpu::IsGpuThread()) {
		const auto waited = std::chrono::duration<double, std::milli>(
		                        std::chrono::steady_clock::now() - wait_begin)
		                        .count();
		static std::atomic<uint32_t> printed {0};
		if (waited >= long_wait_ms && printed.fetch_add(1, std::memory_order_relaxed) < 80u) {
			std::printf("long gpu wait: %.1f ms tick %llu (known %llu)\n", waited,
			            static_cast<unsigned long long>(tick),
			            static_cast<unsigned long long>(KnownGpuTick()));
			Common::ThreadSampler::PrintCurrentStack("  long gpu wait stack");
		}
	}
	if (result != vk::Result::eSuccess) {
		ReportGpuCheckpoints(m_graphics);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	Refresh();
}

} // namespace Libs::Graphics
