#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_

#include "graphics/host_gpu/renderer/gpuCheckpoints.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandScheduler;

// Per-shader GPU time profiler (KYTY_GPU_PROFILE=delay,duration in seconds; "1" = 0,3600).
//
// Every command bracketed by GpuCheckpointBegin/End (draws, direct and indirect dispatches,
// marked emulator copies) gets a bottom-of-pipe timestamp before and after it. The difference is
// that command's incremental GPU time; the rest of each command buffer is "unmarked" (barriers,
// clears, unmarked emulator work). Bottom-of-pipe timestamps serialize the queue, so absolute
// times are inflated for small overlapping work; use the report for attribution, not FPS.
// Results go to gpu-profile.txt in the working directory, rewritten every 5 s while the window
// is open, aggregated by (kind, shader hash) or by source line for emulator work.
[[nodiscard]] bool GpuProfileEnabled();
void               GpuProfileBeginCommandBuffer(CommandScheduler& scheduler, CmdRef command);
void               GpuProfileEndCommandBuffer(CmdRef command, uint64_t tick);
void GpuProfileBegin(CmdRef command, GpuCheckpointOp op, uint64_t key, uint32_t arg0,
                     uint32_t arg1);
void GpuProfileEnd(CmdRef command);
// Tags the next profiled dispatch on this thread as coming from a guest async-compute queue.
void GpuProfileNoteAsyncCompute(bool async_compute);

// KYTY_TRACE_AFTER=hash[,hash...]: after a dispatch of one of these shaders, the next PM4
// packets are printed (first 40 dispatches, 24 packets each) to show the guest's own sync.
void GpuTraceDispatch(uint64_t shader_hash, uint32_t groups_x, uint32_t groups_y,
                      uint32_t groups_z);
void GpuTracePacket(uint32_t opcode, const uint32_t* packet, uint32_t size_dw);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_
