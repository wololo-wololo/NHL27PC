#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>

namespace Libs::Graphics {

struct GraphicContext;

class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_semaphore; }

	// Ticks whose vkQueueSubmit has returned (asynchronous recording hands ticks to a worker that
	// submits later). A submit on another queue that waits on this timeline must wait for its tick
	// to be submitted first: the NVIDIA driver blocks a wait-before-signal submit while holding
	// its device lock, and the worker then cannot submit the signal (gsamp-2 hang, 2026-11-07).
	[[nodiscard]] uint64_t SubmittedTick() const noexcept {
		return m_submitted_tick.load(std::memory_order_acquire);
	}
	void MarkSubmitted(uint64_t tick) noexcept {
		m_submitted_tick.store(tick, std::memory_order_release);
		m_submitted_tick.notify_all();
	}
	// Blocks until `tick` was submitted. Only for ticks already handed to the submitter.
	void WaitSubmitted(uint64_t tick) const noexcept {
		for (auto current = SubmittedTick(); current < tick; current = SubmittedTick()) {
			m_submitted_tick.wait(current, std::memory_order_acquire);
		}
	}

	void Refresh();
	void Wait(uint64_t tick);

private:
	GraphicContext&       m_graphics;
	vk::Semaphore         m_semaphore = nullptr;
	std::atomic<uint64_t> m_gpu_tick {0};
	std::atomic<uint64_t> m_current_tick {1};
	std::atomic<uint64_t> m_submitted_tick {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
