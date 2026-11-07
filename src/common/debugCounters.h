#ifndef KYTY_COMMON_DEBUG_COUNTERS_H_
#define KYTY_COMMON_DEBUG_COUNTERS_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
#include <x86intrin.h>
#endif

namespace Common::DebugCounters {

// The stage timers run millions of times per second; steady_clock (QueryPerformanceCounter on
// Windows) cost ~7% of the GPU thread. The invariant TSC, calibrated once, is far cheaper.
struct FastClock {
	using rep        = int64_t;
	using period     = std::nano;
	using duration   = std::chrono::nanoseconds;
	using time_point = std::chrono::time_point<FastClock>;
	static constexpr bool is_steady = true;

	static double Calibrate() noexcept {
		const auto     begin = std::chrono::steady_clock::now();
		const uint64_t start = __rdtsc();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const auto     end   = std::chrono::steady_clock::now();
		const uint64_t stop  = __rdtsc();
		const double   ns    = static_cast<double>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
		return stop > start ? ns / static_cast<double>(stop - start) : 1.0;
	}
	static time_point now() noexcept {
		static const double ns_per_tick = Calibrate();
		// Never the default (zero) time point, which the timers use as "disabled".
		return time_point(duration(static_cast<int64_t>(static_cast<double>(__rdtsc()) * ns_per_tick) | 1));
	}
};

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

// Collect events only while the panel is visible. Relaxed, lock-free atomics keep updates
// cheap and safe in signal handlers. Rates come from comparing two snapshots.
#define KYTY_DEBUG_COUNTERS(X) \
	/* GPU work. */ \
	X(Draws) \
	X(DrawInstances) \
	X(IndirectDraws) \
	X(Dispatches) \
	X(QueueSubmits) \
	X(QueueSubmitNs) \
	X(Pm4ProcessCalls) \
	X(Pm4ProcessNs) \
	X(Pm4DecodeCalls) \
	X(Pm4DecodeNs) \
	X(BufferFlushCalls) \
	X(BufferFlushNs) \
	X(DrawProcessCalls) \
	X(DrawProcessNs) \
	X(DrawStatePrepCalls) \
	X(DrawStatePrepNs) \
	X(DrawExecuteCalls) \
	X(DrawExecuteNs) \
	X(DispatchProcessCalls) \
	X(DispatchProcessNs) \
	X(BindingImageReuses) \
	X(BindingImageResolves) \
	X(BindingSamplerReuses) \
	X(BindingSamplerResolves) \
	X(TextureMemoHits) /* ResolveTexture answered from its memo (part 12). */ \
	X(TextureMemoMisses) \
	X(TextureMemoMismatches) /* KYTY_TEXTURE_MEMO_VERIFY=1: memo differs from a full resolve. */ \
	X(TargetMemoHits) /* Render-target memo reused its image without FindImage (part 12). */ \
	X(TargetMemoMisses) \
	X(AgcHelperPackets) /* Packets produced through native Graphics5 command-buffer helpers. */ \
	X(AgcHelperDwords) \
	X(AgcDcbSubmits) /* Guest graphics command-buffer submissions. */ \
	X(AgcDcbDwords) \
	X(AgcDcbSubmitNs) \
	X(AgcAcbSubmits) /* Guest async-compute command-buffer submissions. */ \
	X(AgcAcbDwords) \
	X(AgcAcbSubmitNs) \
	X(GpuWaits) /* CPU blocked on a GPU timeline semaphore. */ \
	X(GpuWaitNs) \
	X(GpuThreadWaits) /* The subset of GpuWaits made by the guest GPU (PM4) thread. */ \
	X(GpuThreadWaitNs) \
	X(GpuThreadSyncWaitNs) /* GPU thread blocked waiting for priority operations (publishes). */ \
	X(MaterializeFaultCalls) /* Materializations that raised host exceptions (walker loads). */ \
	X(MaterializeFaultNs) \
	X(MaterializeFaultExceptions) \
	X(MaterializeCleanNs) \
	X(WalkSrtNs) /* JIT SRT walker runs (inside materialization). */ \
	X(LoopWatchdogsEmitted) /* Loop headers given the iteration budget (shader compiles). */ \
	X(MirrorBytes) /* Guest memory added to the GPU mirror (KYTY_GPU_MIRROR). */ \
	X(AsyncMaterializePosted) /* Pixel-shader materializations posted to the helper. */ \
	X(AsyncMaterializeUsed) /* ... whose helper result the lookup used. */ \
	X(AsyncMaterializeAborted) /* ... aborted by a walker fault (redone inline). */ \
	X(AsyncMaterializeWaitNs) /* GPU thread waiting for the helper. */ \
	X(LoopWatchdogLoops) /* Structured loops in compiled shaders (guarded or not). */ \
	X(ImageCreates) /* Host images created (GraphicContext::CreateImage). */ \
	X(ImageCreateNs) \
	X(ImageDeletes) \
	X(SkippedDraws) /* Draws dropped by KYTY_SKIP_DRAWS (diagnostic). */ \
	X(ImageDeleteNs) \
	X(GpuThreadReadbackNs) /* Buffer readbacks (ReadMemory sync) made on the GPU thread. */ \
	X(GpuFaults) /* Guest writes to GPU-tracked memory. */ \
	X(GpuFaultNs) \
	X(GpuFaultReads) \
	X(GpuFaultWrites) \
	X(GpuFaultMappedNs) \
	X(GpuFaultBufferNs) \
	X(GpuFaultBufferTrackerNs) \
	X(GpuFaultBdaSyncNs) \
	X(BufferReadCalls) \
	X(BufferReadQueuedCalls) \
	X(BufferReadDirtyCalls) \
	X(BufferReadDownloadCalls) \
	X(BufferReadSyncNs) \
	X(BufferReadBodyNs) \
	X(BufferReadDownloadNs) \
	/* Downloads forced by CPU writes to pages holding GPU-written bytes (write faults). */ \
	X(CpuOverGpuUploads) /* CPU-dirty uploads overlapping undownloaded GPU-written bytes */ \
	X(CpuOverGpuUploadBytes) \
	/* Readback producers: latest GPU write to the downloaded bytes is in the recording command */ \
	/* buffer, submitted but not complete, already complete, or unknown. Rb* = reads, */ \
	/* RbWrite* = write faults; AgeTicks sums current - producer tick. */ \
	X(RbProducerRecording) \
	X(RbProducerInFlight) \
	X(RbProducerDone) \
	X(RbProducerUnknown) \
	X(RbProducerAgeTicks) \
	X(RbWriteProducerRecording) \
	X(RbWriteProducerInFlight) \
	X(RbWriteProducerDone) \
	X(RbWriteProducerUnknown) \
	X(RbWriteProducerAgeTicks) \
	X(SideReadbacks) /* downloads on the side queue (producer already submitted) */ \
	X(SideReadbackNs) \
	/* Waits for a tick to be submitted by the recording worker (MasterSemaphore::WaitSubmitted); */ \
	/* counted only when the tick was not submitted yet. TicksBehind sums the shortfall. */ \
	X(SideWaitSubmitted) \
	X(SideWaitSubmittedNs) \
	X(PresentWaitSubmitted) \
	X(PresentWaitSubmittedNs) \
	X(PresentWaitTicksBehind) \
	X(PresentWaitSkipped) /* KYTY_PRESENT_WAIT_SUBMITTED=0/ab skipped a needed wait */ \
	X(PresentCalls) /* Presenter::Present, whole call (presenter thread) */ \
	X(PresentNs) \
	X(BdaSliceSyncs) /* RenderContext::PrepareBdaForSubmission (GPU thread, outside Pm4ProcessNs) */ \
	X(BdaSliceSyncNs) \
	X(DriverCacheSaves) /* Vulkan driver pipeline cache written to disk (background thread) */ \
	X(DriverCacheSaveNs) \
	X(ProgramMisses) /* PipelineCache::GetProgram misses: prepare + translate + compile */ \
	X(ProgramMissNs) \
	X(UnmapDrains) /* RenderContext::UnmapMemory waits (GPU thread) */ \
	X(UnmapDrainNs) \
	X(UnmapDrainsSkipped) /* KYTY_UNMAP_DRAIN=priority: drains the old rule would have made */ \
	X(WriteFaultSyncCalls) \
	X(WriteFaultSyncNs) \
	X(WriteFaultDownloadCalls) \
	X(WriteFaultDownloadBytes) \
	X(WriteFaultDownloadNs) \
	X(GpuFaultTextureNs) \
	X(GpuFaultImageHeapNs) \
	X(ShadersCompiled) \
	X(ShaderCompileNs) \
	X(PipelinesCreated) \
	X(PipelineCreateNs) \
	/* Caches. */ \
	X(TextureUploadBytes) \
	X(TextureUploadsGpu) /* re-uploads because GPU buffer writes hit the image memory */ \
	X(TextureUploadGpuBytes) \
	X(TextureUploadsCpu) /* uploads of CPU-written image memory */ \
	X(TextureUploadCpuBytes) \
	X(TextureDownloadBytes) \
	X(TextureWriteFaultDownloads) /* GPU-written images written back before a CPU write */ \
	X(TextureOverlapPublishes) /* GPU-written images written back before an overlapping new image */ \
	X(TextureEvictions) \
	X(ImagesMipClamped) /* images created with fewer host levels than the guest declared */ \
	X(ImageCopiesMipSkipped) /* upload/download regions of levels beyond the host chain */ \
	X(ColorMetadataReadbacks) /* CPU blocked reading back GPU-written DCC/CMASK metadata. */ \
	X(ColorMetadataReadbackNs) \
	X(ColorMetadataChecks) /* GPU clear-key checks of DCC/CMASK metadata. */ \
	X(BufferUploadBytes) \
	X(BufferSyncUploads) /* SynchronizeBuffer copies (each with a full barrier pair) */ \
	X(BufferSyncUploadsWritten) /* ... for a buffer the shader writes */ \
	X(BufferSyncUploadsSmall) /* ... of at most 64 KiB */ \
	X(BufferSyncUploadsLarge) /* ... of more than 1 MiB */ \
	X(BufferSyncUploadsInPass) /* ... that ended a render pass */ \
	X(BufferSyncUploadsBackToBack) /* ... with nothing recorded since the previous one */ \
	X(BufferSyncUploadsGpuDirty) /* ... into a buffer holding GPU-written bytes */ \
	X(BufferSyncUploadBatches) /* barrier pairs recorded for batched uploads */ \
	X(RenderPassBegins) /* vkCmdBeginRendering calls */ \
	X(DrawWriteHazards) /* draws with buffer writes: render pass end + full hazard barrier */ \
	X(BufferDownloadBytes) \
	X(BufferEvictions) \
	/* Presentation. */ \
	X(FlipsRequested) \
	X(FlipsRejected) /* Guest flip queue was full. */ \
	X(LateVblanks) /* The present thread fell behind the vblank rate. */ \
	X(SwapchainRecreations) \
	X(DroppedPresents) \
	/* Emulator. */ \
	X(EmulatedInstructions) /* Host CPU lacked an instruction the guest used. */ \
	X(UnresolvedImportCalls) \
	X(GuestFileReadBytes) \
	X(GuestFileWriteBytes) \
	X(AudioUnderruns) \
	/* Compute dispatch stages. */ \
	X(CsProgramNs) \
	X(CsFastPathNs) \
	X(CsPipelineNs) \
	X(CsFindBuffersNs) \
	X(CsBdaNs) \
	X(CsImageHeapNs) \
	X(CsImagesNs) \
	X(CsBuffersNs) \
	X(CsCommitNs) \
	X(CsRecordNs) \
	/* Draw stages. */ \
	X(DrawFindBuffersNs) \
	X(DrawRecordNs) \
	X(DrawImagesNs) \
	X(DrawBuffersNs) \
	X(DrawCommitNs) \
	X(DrawPipelineNs) \
	X(PopPendingNs) 	/* Relaxed compute barriers (KYTY_RELAXED_CS_BARRIERS). */ 	X(CsBarriersDeferred) 	X(CsBarriersElided) /* Dispatch-to-dispatch barriers left to guest synchronization. */ 	X(CsHazardsElided) 	X(CsBarriersFlushed) 	X(CsGuestSyncFlushes) 	/* Shader program lookup. */ 	X(DrawProgramsNs) 	X(DrawTargetsNs) 	X(ProgramKeyNs) 	X(ProgramMaterializeNs) 	X(ProgramMatchNs) 	X(ProgramLookups) 	X(MaterializeHits) 	X(MaterializeMisses) 	X(MissFlat) 	X(MissDescriptors) 	X(MissDescriptorBaseOnly) /* Only DWORDs 0-1 (buffer base) of descriptors differ. */ 	X(MissActive) 	X(MissOutputs) 	X(MissInternal) /* Only walker slots or captured addresses differ. */ 	/* GPU interrupts (RELEASE_MEM end-of-pipe events). */ 	X(InterruptsQueued) 	X(InterruptsDelivered) 	X(InterruptsUnregistered) /* Delivered while no event queue was registered for the id. */ 	/* Page protection and graphics binding preparation. */ 	X(ProtectCalls) 	X(ProtectNs) 	X(BindFindNs) 	X(BindBdaNs) 	X(BindImageHeapNs) 	X(BindImagesNs) 	X(BindTargetsNs) 	X(BindBuffersNs) \
	X(WalkerLoadsEmulated) /* SRT walker loads completed from the backing store. */ \
	X(WalkerLoadsUnknown) /* condition-only walker loads of GPU-written bytes left unknown */ \
	/* GPU timestamps around submitted command buffers (KYTY_GPU_TIMING=1). */ \
	X(GpuTimedSubmits) \
	X(GpuBusyNs) \
	X(GpuGapNs) /* GPU idle between consecutive submissions. */ \
	/* end */

enum class Counter {
#define KYTY_DEBUG_COUNTER_ENUM(name) name,
	KYTY_DEBUG_COUNTERS(KYTY_DEBUG_COUNTER_ENUM)
#undef KYTY_DEBUG_COUNTER_ENUM
	Count
};

enum class Peak { ShaderCompileNs, PipelineCreateNs, Count };

namespace Detail {
inline std::atomic<bool>     g_enabled {false};
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
inline std::atomic<uint64_t> g_peaks[static_cast<int>(Peak::Count)] {};
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

inline void SetEnabled(bool enabled) noexcept {
	Detail::g_enabled.store(enabled, std::memory_order_relaxed);
}

// Host access-violation exceptions handled on the current thread (any kind).
inline thread_local uint32_t t_host_exceptions = 0;

inline void Add(Counter counter, uint64_t value = 1) noexcept {
	if (!Enabled()) {
		return;
	}
	Detail::g_counters[static_cast<int>(counter)].fetch_add(value, std::memory_order_relaxed);
}

class ScopedTimer final {
public:
	ScopedTimer(Counter count, Counter duration, Peak peak = Peak::Count)
	    : m_count(count), m_duration(duration), m_peak(peak),
	      m_start(Enabled() ? FastClock::now()
	                        : FastClock::time_point {}) {}
	~ScopedTimer() {
		if (m_start == FastClock::time_point {} || !Enabled()) {
			return;
		}
		const auto elapsed =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              FastClock::now() - m_start)
		                              .count());
		Add(m_count);
		Add(m_duration, elapsed);
		if (m_peak != Peak::Count) {
			auto& peak     = Detail::g_peaks[static_cast<int>(m_peak)];
			auto  previous = peak.load(std::memory_order_relaxed);
			while (previous < elapsed &&
			       !peak.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {
			}
		}
	}
	ScopedTimer(const ScopedTimer&)            = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;
	void Cancel() noexcept { m_start = {}; }

private:
	Counter                               m_count;
	Counter                               m_duration;
	Peak                                  m_peak;
	FastClock::time_point m_start;
};

[[nodiscard]] inline uint64_t Get(Counter counter) noexcept {
	return Detail::g_counters[static_cast<int>(counter)].load(std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t Get(Peak peak) noexcept {
	return Detail::g_peaks[static_cast<int>(peak)].load(std::memory_order_relaxed);
}

[[nodiscard]] const char* Name(Counter counter) noexcept;

// Accumulates only elapsed time into one counter; for sub-stages whose calls equal the parent's.
class StageTimer final {
public:
	explicit StageTimer(Counter duration)
	    : m_duration(duration), m_start(Enabled() ? FastClock::now()
	                                              : FastClock::time_point {}) {}
	~StageTimer() {
		if (m_start == FastClock::time_point {}) {
			return;
		}
		Add(m_duration, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                          FastClock::now() - m_start)
		                                          .count()));
	}
	StageTimer(const StageTimer&)            = delete;
	StageTimer& operator=(const StageTimer&) = delete;

private:
	Counter                               m_duration;
	FastClock::time_point m_start;
};

// Splits a function into consecutive stages; each Next() charges the time since the previous
// boundary to the stage that just ended.
class StageSequence final {
public:
	explicit StageSequence(Counter first)
	    : m_current(first), m_start(Enabled() ? FastClock::now()
	                                          : FastClock::time_point {}) {}
	~StageSequence() { Flush(FastClock::time_point {}); }
	void Next(Counter stage) {
		if (m_start == FastClock::time_point {}) {
			return;
		}
		Flush(FastClock::now());
		m_current = stage;
	}
	StageSequence(const StageSequence&)            = delete;
	StageSequence& operator=(const StageSequence&) = delete;

private:
	void Flush(FastClock::time_point next_start) {
		if (m_start == FastClock::time_point {}) {
			return;
		}
		const auto now = next_start == FastClock::time_point {}
		                     ? FastClock::now()
		                     : next_start;
		Add(m_current, static_cast<uint64_t>(
		                   std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start)
		                       .count()));
		m_start = next_start;
	}

	Counter                               m_current;
	FastClock::time_point m_start;
};

// Time spent per PM4 type-3 opcode, inclusive of the host work the packet handler performs.
namespace Detail {
// Slots 256 and up hold Kyty's custom sub-operations carried in NOP packets.
inline constexpr uint32_t    OPCODE_SLOTS = 256 + 64;
inline std::atomic<uint64_t> g_opcode_calls[OPCODE_SLOTS] {};
inline std::atomic<uint64_t> g_opcode_ns[OPCODE_SLOTS] {};
inline std::atomic<uint64_t> g_opcode_readback_ns[OPCODE_SLOTS] {};
// The packet the graphics thread is executing, for attributing nested costs.
inline thread_local uint32_t g_current_opcode = OPCODE_SLOTS;
} // namespace Detail

inline void SetCurrentOpcode(uint32_t slot) noexcept {
	Detail::g_current_opcode = slot;
}

// Charges synchronous readback time to the packet that caused it.
inline void AddReadbackToCurrentOpcode(uint64_t ns) noexcept {
	const auto slot = Detail::g_current_opcode;
	if (slot < Detail::OPCODE_SLOTS) {
		Detail::g_opcode_readback_ns[slot].fetch_add(ns, std::memory_order_relaxed);
	}
}

[[nodiscard]] inline uint64_t GetOpcodeReadbackNs(uint32_t slot) noexcept {
	return slot < Detail::OPCODE_SLOTS
	           ? Detail::g_opcode_readback_ns[slot].load(std::memory_order_relaxed)
	           : 0;
}

inline void AddOpcode(uint32_t slot, uint64_t ns) noexcept {
	if (slot >= Detail::OPCODE_SLOTS) {
		return;
	}
	Detail::g_opcode_calls[slot].fetch_add(1, std::memory_order_relaxed);
	Detail::g_opcode_ns[slot].fetch_add(ns, std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t GetOpcodeCalls(uint32_t slot) noexcept {
	return slot < Detail::OPCODE_SLOTS
	           ? Detail::g_opcode_calls[slot].load(std::memory_order_relaxed)
	           : 0;
}

[[nodiscard]] inline uint64_t GetOpcodeNs(uint32_t slot) noexcept {
	return slot < Detail::OPCODE_SLOTS ? Detail::g_opcode_ns[slot].load(std::memory_order_relaxed)
	                                   : 0;
}

// Times a scope into one opcode slot.
class OpcodeTimer final {
public:
	explicit OpcodeTimer(uint32_t slot)
	    : m_slot(slot), m_start(Enabled() ? FastClock::now()
	                                      : FastClock::time_point {}) {}
	~OpcodeTimer() {
		if (m_start == FastClock::time_point {}) {
			return;
		}
		AddOpcode(m_slot, static_cast<uint64_t>(
		                      std::chrono::duration_cast<std::chrono::nanoseconds>(
		                          FastClock::now() - m_start)
		                          .count()));
	}
	OpcodeTimer(const OpcodeTimer&)            = delete;
	OpcodeTimer& operator=(const OpcodeTimer&) = delete;

private:
	uint32_t                              m_slot;
	FastClock::time_point m_start;
};

// In-run A/B testing: KYTY_AB_PERIOD=N alternates the variant every N seconds and tags each
// logged second with ab=0/1. Optimizations under test take their old path while it is false;
// without the variable it stays true.
namespace Detail {
inline std::atomic<bool> g_ab_variant {true};
} // namespace Detail

[[nodiscard]] inline bool AbVariant() noexcept {
	return Detail::g_ab_variant.load(std::memory_order_relaxed);
}

// When KYTY_PERF_CSV names a file, enables collection and appends one line of per-second
// rates each second, so runs can be compared without reading the overlay.
void StartCsvLoggerFromEnvironment();
void StopCsvLogger();

} // namespace Common::DebugCounters

#endif /* KYTY_COMMON_DEBUG_COUNTERS_H_ */
