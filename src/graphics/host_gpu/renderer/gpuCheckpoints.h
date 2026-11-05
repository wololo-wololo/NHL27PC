#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCHECKPOINTS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCHECKPOINTS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

// GPU buffer markers (VK_AMD_buffer_marker): the GPU writes each marked command's sequence
// number to host memory before and after it runs, so after a device loss the last started and
// last completed numbers name the commands that were in flight.
//
// By default indirect dispatches and the emulator's own copies and internal dispatches are
// marked, which is cheap and makes every device loss report where the GPU stopped.
// KYTY_INDIRECT_MARKERS=0 turns markers off; KYTY_GPU_CHECKPOINTS=1 marks every dispatch and
// draw as well (KYTY_GPU_CHECKPOINTS_MASK selects kinds by bit). Marking every compute dispatch
// makes NHL 27's intermittent device loss much rarer, so it hides rather than shows that hang.
// KYTY_GPU_CHECKPOINTS_DISPATCH_EVERY=N and KYTY_GPU_CHECKPOINTS_DISPATCH_PHASE=P reduce only
// direct-dispatch markers to ordinals where ordinal % N == P.
enum class GpuCheckpointOp : uint32_t { Dispatch, DispatchIndirect, Draw, Copy };

[[nodiscard]] bool GpuCheckpointsRequested();
// Set before device creation when VK_EXT_device_fault is enabled.
void               SetDeviceFaultReporting(bool enabled);
// Creates the marker buffer after the device exists; leaves checkpoints off on failure.
void               InitGpuCheckpoints(GraphicContext& graphics);
[[nodiscard]] bool GpuCheckpointsEnabled();

// Returns the record's sequence number, for the matching end checkpoint.
uint64_t GpuCheckpointBegin(vk::CommandBuffer command, GpuCheckpointOp op, uint64_t shader_hash,
                            uint32_t arg0 = 0, uint32_t arg1 = 0, uint32_t arg2 = 0,
                            uint64_t address = 0, uint64_t address2 = 0,
                            bool async_compute = false);
void     GpuCheckpointEnd(vk::CommandBuffer command, uint64_t sequence);

// Marks the emulator's own GPU work (copies, clears, internal dispatches) until the scope ends;
// the report names its source line.
class GpuCheckpointScope {
public:
	GpuCheckpointScope(vk::CommandBuffer command, const char* file, int line)
	    : m_command(command),
	      m_sequence(GpuCheckpointBegin(command, GpuCheckpointOp::Copy,
	                                    reinterpret_cast<uint64_t>(file),
	                                    static_cast<uint32_t>(line))) {}
	// With details printed in the device-loss report (e.g. sizes and addresses).
	GpuCheckpointScope(vk::CommandBuffer command, const char* file, int line, uint32_t detail0,
	                   uint32_t detail1, uint64_t detail2, uint64_t detail3)
	    : m_command(command),
	      m_sequence(GpuCheckpointBegin(command, GpuCheckpointOp::Copy,
	                                    reinterpret_cast<uint64_t>(file),
	                                    static_cast<uint32_t>(line), detail0, detail1, detail2,
	                                    detail3)) {}
	~GpuCheckpointScope() { GpuCheckpointEnd(m_command, m_sequence); }
	GpuCheckpointScope(const GpuCheckpointScope&)            = delete;
	GpuCheckpointScope& operator=(const GpuCheckpointScope&) = delete;

private:
	vk::CommandBuffer m_command;
	uint64_t          m_sequence;
};

// KYTY_GPU_LOG_INDIRECT=1 (with markers): copies an indirect draw's argument record into
// device-loss-safe host memory right before the draw. Must be recorded outside rendering.
[[nodiscard]] bool GpuIndirectArgsLogEnabled();
// Emulator-side view of the record when the draw was recorded.
struct GpuIndirectArgsHostState {
	uint32_t backing[5]    = {};
	bool     backing_valid = false;
	bool     gpu_dirty     = false;
	bool     cpu_dirty     = false;
	uint32_t count         = 0;
	uint32_t stride        = 0;
	uint64_t source_size   = 0;
	uint64_t tick          = 0;
	uint64_t command       = 0;
};
void GpuLogIndirectArgs(vk::CommandBuffer command, vk::Buffer source, uint64_t offset,
                        uint64_t guest_address, uint32_t bytes,
                        const GpuIndirectArgsHostState& host);

// Diagnostics: the dispatch being prepared on this thread (shader hash and group counts).
struct GpuCurrentDispatch {
	uint64_t shader_hash = 0;
	uint32_t groups[3]   = {};
	bool     indirect    = false;
};
void                      GpuNoteCurrentDispatch(const GpuCurrentDispatch& dispatch);
const GpuCurrentDispatch& GpuGetCurrentDispatch();

// Reports a draw or dispatch whose commands went into a Vulkan buffer that was submitted while
// the command was being prepared (those commands never execute).
void ReportRetiredRecording(const char* what, uint64_t shader_hash);

// Why the next texture upload on this thread happens (bit 0: GPU buffer writes to the image's
// memory, bit 1: CPU writes); recorded in its checkpoint detail.
void     GpuCheckpointSetUploadReason(uint32_t reason);
uint32_t GpuCheckpointTakeUploadReason();

// Prints the queue's checkpoints with their records; call after a device loss.
void ReportGpuCheckpoints(const GraphicContext& graphics);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUCHECKPOINTS_H_
