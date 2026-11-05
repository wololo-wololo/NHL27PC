#ifndef KYTY_COMMON_DEBUG_COUNTERS_H_
#define KYTY_COMMON_DEBUG_COUNTERS_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Common::DebugCounters {

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
	      m_start(Enabled() ? std::chrono::steady_clock::now()
	                        : std::chrono::steady_clock::time_point {}) {}
	~ScopedTimer() {
		if (m_start == std::chrono::steady_clock::time_point {} || !Enabled()) {
			return;
		}
		const auto elapsed =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              std::chrono::steady_clock::now() - m_start)
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
	std::chrono::steady_clock::time_point m_start;
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
	    : m_duration(duration), m_start(Enabled() ? std::chrono::steady_clock::now()
	                                              : std::chrono::steady_clock::time_point {}) {}
	~StageTimer() {
		if (m_start == std::chrono::steady_clock::time_point {}) {
			return;
		}
		Add(m_duration, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                          std::chrono::steady_clock::now() - m_start)
		                                          .count()));
	}
	StageTimer(const StageTimer&)            = delete;
	StageTimer& operator=(const StageTimer&) = delete;

private:
	Counter                               m_duration;
	std::chrono::steady_clock::time_point m_start;
};

// Splits a function into consecutive stages; each Next() charges the time since the previous
// boundary to the stage that just ended.
class StageSequence final {
public:
	explicit StageSequence(Counter first)
	    : m_current(first), m_start(Enabled() ? std::chrono::steady_clock::now()
	                                          : std::chrono::steady_clock::time_point {}) {}
	~StageSequence() { Flush(std::chrono::steady_clock::time_point {}); }
	void Next(Counter stage) {
		if (m_start == std::chrono::steady_clock::time_point {}) {
			return;
		}
		Flush(std::chrono::steady_clock::now());
		m_current = stage;
	}
	StageSequence(const StageSequence&)            = delete;
	StageSequence& operator=(const StageSequence&) = delete;

private:
	void Flush(std::chrono::steady_clock::time_point next_start) {
		if (m_start == std::chrono::steady_clock::time_point {}) {
			return;
		}
		const auto now = next_start == std::chrono::steady_clock::time_point {}
		                     ? std::chrono::steady_clock::now()
		                     : next_start;
		Add(m_current, static_cast<uint64_t>(
		                   std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start)
		                       .count()));
		m_start = next_start;
	}

	Counter                               m_current;
	std::chrono::steady_clock::time_point m_start;
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
	    : m_slot(slot), m_start(Enabled() ? std::chrono::steady_clock::now()
	                                      : std::chrono::steady_clock::time_point {}) {}
	~OpcodeTimer() {
		if (m_start == std::chrono::steady_clock::time_point {}) {
			return;
		}
		AddOpcode(m_slot, static_cast<uint64_t>(
		                      std::chrono::duration_cast<std::chrono::nanoseconds>(
		                          std::chrono::steady_clock::now() - m_start)
		                          .count()));
	}
	OpcodeTimer(const OpcodeTimer&)            = delete;
	OpcodeTimer& operator=(const OpcodeTimer&) = delete;

private:
	uint32_t                              m_slot;
	std::chrono::steady_clock::time_point m_start;
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
