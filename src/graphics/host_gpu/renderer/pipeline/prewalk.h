#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PREWALK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PREWALK_H_

#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <cstdint>
#include <span>
#include <vector>

// Look-ahead materialization (2026-11-07 part 13; on, KYTY_PREWALK=0 disables; follows
// KYTY_AB_PERIOD, KYTY_AB_TARGET=prewalk).
//
// At the start of every submission slice the GPU thread hands a helper thread a copy of the
// command processor's shader registers and the PM4 still to run. The helper replays only the SH
// register packets on a shadow command processor and, for each draw (vertex and pixel program) and
// direct dispatch, materializes the program's shader resources (SRT walk, descriptor decode,
// specialization) with its own walker scratch, recording every guest address it read. Walker
// loads that fault (GPU-written pages) abort the job; it never touches the buffer or texture
// caches.
//
// When the GPU thread later materializes a program, it takes a ready helper result for the same
// program, shader base and user data if none of the recorded reads lies on memory the GPU thread
// changed in this slice (host_gpu/sliceWriteLog.h). CPU writes during a slice do not matter: the
// guest relies only on CPU writes made before it submitted or before a label the command buffer
// waits on, which ends the slice. The register replay is only a prediction; a wrong one is a miss.
// KYTY_PREWALK_VERIFY=1 recomputes every taken result on the GPU thread and counts differences.

namespace Libs::Graphics {

class CommandProcessor;
class Pm4Execution;
class RenderContext;
struct ShaderParams;

namespace Prewalk {

enum class Stage : uint8_t { Vertex, Pixel, Compute };

[[nodiscard]] bool Enabled();

// GPU thread, after a program lookup for a draw (logical vertex or pixel stage) or a dispatch.
void Publish(Stage stage, PipelineCache::Program& program, const ShaderParams& params);

// GPU thread, at the start of a graphics or compute submission slice, before any packet runs.
void BeginSlice(CommandProcessor& cp, RenderContext& context, std::vector<std::span<const uint32_t>> pending);

// GPU thread, Program::Materialize: true when `program.resources`/`specialization` now hold a
// helper result for exactly this runtime.
[[nodiscard]] bool Take(PipelineCache::Program& program, const ShaderRecompiler::IR::SrtRuntime& runtime);

} // namespace Prewalk
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PREWALK_H_
