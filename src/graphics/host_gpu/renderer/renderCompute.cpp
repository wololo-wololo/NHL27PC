#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/gpuCheckpoints.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {
// KYTY_RELAXED_CS_BARRIERS=1 leaves dispatch-to-dispatch ordering to the guest's own
// synchronization packets instead of a full barrier after every dispatch.
bool RelaxedComputeBarriers() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_RELAXED_CS_BARRIERS");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

// Guest address of the first written (or else first) bound image, for checkpoint records.
static uint64_t FirstImageAddress(const ShaderModule& module, const PreparedBindings& bindings,
                                  bool written) {
	const auto& images = module.info.info.images;
	for (size_t i = 0; i < bindings.images.size() && i < images.size(); i++) {
		if (!written || images[i].written) {
			const auto address = bindings.images[i].desc.info.data.address;
			if (address != 0) {
				return address;
			}
		}
	}
	return 0;
}

// On by default (2026-11-05: 11 chaining runs without device loss, +25-32% FPS); =0 disables.
static bool ChainSingleGroupDispatches() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_CHAIN_SINGLE_GROUP");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// KYTY_CHAIN_SHADERS=hash[,hash...] limits KYTY_CHAIN_SINGLE_GROUP to these shaders ("all" for
// every single-workgroup dispatch). Default: NHL 27's two cloth/physics solver shaders.
static bool ChainShaderAllowed(uint64_t shader_hash) {
	static const std::vector<uint64_t> hashes = [] {
		std::vector<uint64_t> list;
		const char* value = std::getenv("KYTY_CHAIN_SHADERS");
		if (value == nullptr) {
			value = "0813aca5017577ac,2edf88de3cdb2ee3";
		} else if (std::strcmp(value, "all") == 0) {
			value = nullptr;
		}
		for (const char* p = value; p != nullptr && *p != '\0';) {
			char*          end  = nullptr;
			const uint64_t hash = std::strtoull(p, &end, 16);
			if (end == p) {
				break;
			}
			list.push_back(hash);
			p = *end == ',' ? end + 1 : end;
		}
		return list;
	}();
	return hashes.empty() || std::find(hashes.begin(), hashes.end(), shader_hash) != hashes.end();
}

// KYTY_DIAG_INDIRECT=1 reads every indirect dispatch's group counts before recording it (a
// synchronous readback when they are GPU-written) and logs implausible ones.
static bool IndirectDiagnosticsEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DIAG_INDIRECT");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

static void LogIndirectDispatch(RenderContext& context, uint64_t shader_hash, uint64_t args_addr,
                                const uint32_t* threads) {
	vk::DispatchIndirectCommand args {};
	if (!LibKernel::Memory::TryReadBufferBacking(args_addr, &args, sizeof(args))) {
		return;
	}
	const auto& limits = context.GetGraphics().GetPhysicalDeviceProperties().limits;
	const uint64_t groups = uint64_t {args.x} * args.y * args.z;
	const bool over_limit = args.x > limits.maxComputeWorkGroupCount[0] ||
	                        args.y > limits.maxComputeWorkGroupCount[1] ||
	                        args.z > limits.maxComputeWorkGroupCount[2];
	static std::mutex                             mutex;
	static std::unordered_map<uint64_t, uint32_t> seen;
	std::scoped_lock                              lock(mutex);
	auto&                                         count = seen[shader_hash];
	if (over_limit || groups > (1u << 24u) || count < 3) {
		std::printf("IndirectDispatch shader=0x%016" PRIx64 " args@0x%012" PRIx64
		            " groups=%ux%ux%u local=%ux%ux%u%s%s\n",
		            shader_hash, args_addr, args.x, args.y, args.z, threads[0], threads[1],
		            threads[2], over_limit ? " OVER-DEVICE-LIMIT" : "",
		            groups > (1u << 24u) ? " HUGE" : "");
		std::fflush(stdout);
	}
	count++;
}

static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const CommandBuffer&          buffer) {
	const auto& program   = input.stage.module->info;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (program.info.has_bitwise_xor) {
		return false;
	}
	// Only a dispatch whose single written buffer is exactly a live depth image's registered
	// metadata range is consumed as a metadata clear. Matching on the base address alone also
	// consumed unrelated multi-buffer passes whose output buffer happened to start at a metadata
	// base (NHL 27: e.g. 0x7fd524a7ac97dd14 writing 0x355000 bytes over a 0x8000 HTILE range);
	// those dispatches were dropped and their consumers read stale data (host GPU timeouts).
	uint32_t written_count = 0;
	uint32_t written_index = 0;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (program.info.buffers[i].written) {
			written_count++;
			written_index = i;
		}
	}
	if (written_count == 0) {
		return false;
	}
	static const bool log_meta = std::getenv("KYTY_CHECK_GRID_UPLOADS") != nullptr;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (!program.info.buffers[i].written) {
			continue;
		}
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto base       = descriptor.Base48();
		if (!cache.IsMeta(base)) {
			continue;
		}
		const auto state = cache.DescribeMeta(base);
		const bool exact = written_count == 1 && i == written_index && state.owner_live &&
		                   state.owner_matches && descriptor.GetSize() == state.size;
		if (log_meta && !exact) {
			static std::atomic<uint32_t> logged {0};
			if (logged.fetch_add(1, std::memory_order_relaxed) < 100) {
				std::printf("meta clear not consumed: shader=0x%016" PRIx64 " base=0x%016" PRIx64
				            " written=0x%" PRIx64 " registered=0x%" PRIx64
				            " written_buffers=%u owner_live=%d owner_matches=%d\n",
				            program.shader_hash, base, descriptor.GetSize(), state.size,
				            written_count, state.owner_live ? 1 : 0, state.owner_matches ? 1 : 0);
			}
		}
		if (exact) {
			return cache.ClearMeta(base);
		}
	}
	return false;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	// Offset-or-swizzle bounds do not describe the element range proved by the shader.
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.OutOfBounds() == 3 || descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	const auto elements = descriptor.NumRecords();
	const bool covered = fill.bounded_elements != 0
	                         ? fill.bounded_elements == elements && invocations >= elements
	                         : invocations == elements;
	if (!covered || invocations > uint64_t {UINT32_MAX} + 1 || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeFill(const ShaderComputeInputInfo& input,
                                          CommandBuffer& command, uint32_t group_x,
                                          uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = input.stage.module->info;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		cache.ClearImage(command, binding.image_id, image.backing.format, range, clear);
		return true;
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	const bool image_clear =
	    cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear);
	static const bool log_fills = std::getenv("KYTY_CHECK_GRID_UPLOADS") != nullptr;
	if (log_fills && size >= (1u << 20u)) {
		std::printf("compute fill 0x%016" PRIx64 "+0x%" PRIx64 " value=0x%08x path=%s\n",
		            descriptor.Base48(), size, packed_clear,
		            image_clear ? "image-clear"
		            : (descriptor.Base48() & 3u) != 0 || !m_context.IsMapped(descriptor.Base48(), size) ||
		                    cache.IsRegionRegistered(descriptor.Base48(), size)
		                ? "native-dispatch"
		                : "cpu-fill");
	}
	if (!image_clear) {
		// CPU fills invalidate overlapping cached images. Preserve the native dispatch for
		// partial or ambiguous image backings, including pooled metadata aliases.
		if ((descriptor.Base48() & 3u) != 0 ||
		    !m_context.IsMapped(descriptor.Base48(), size) ||
		    cache.IsRegionRegistered(descriptor.Base48(), size)) {
			return false;
		}
		// Metadata need not have an image backing yet. Keep proven fills in guest memory
		// when CPU-owned, so color-clear discovery can read them without a GPU download.
		m_context.GetBufferCache().FillBuffer(descriptor.Base48(), size, packed_clear, false);
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute %s shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     image_clear ? "image clear" : "buffer fill", input.stage.module->info.shader_hash,
		     descriptor.Base48(), size, packed_clear);
	}
	return true;
}

static void BindSharedMemory(RenderContext& context, ShaderComputeInputInfo& input,
                             PreparedBindings& bindings, uint64_t indirect_args = 0) {
	if (input.stage.module->info.bindings.descriptor_counts[
	        static_cast<size_t>(ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory)] == 0) {
		return;
	}
	auto& cache = context.GetBufferCache();
	if (indirect_args != 0) {
		cache.ReadMemory(indirect_args, sizeof(vk::DispatchIndirectCommand));
		std::memcpy(input.workgroup_counts, reinterpret_cast<const void*>(indirect_args),
		            sizeof(input.workgroup_counts));
	}
	// LDS has no contents to preserve between dispatches. The existing shader hazard
	// barriers also order other users of this GPU-only utility buffer.
	auto& storage = cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
	const auto limit = std::min<uint64_t>(storage.Size(),
	    context.GetGraphics().GetPhysicalDeviceProperties().limits.maxStorageBufferRange);
	uint64_t size = sizeof(uint32_t);
	if (std::ranges::find(input.workgroup_counts, 0u) == std::end(input.workgroup_counts)) {
		size = uint64_t {input.lds_size_dwords} * sizeof(uint32_t);
		EXIT_IF(size == 0 || size > limit);
		for (const auto count: input.workgroup_counts) {
			EXIT_IF(size > limit / count);
			size *= count;
		}
	}
	bindings.shared_memory = {storage.Handle(), 0, size};
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode, bool async_compute) {
	Common::DebugCounters::ScopedTimer timer(
	    Common::DebugCounters::Counter::DispatchProcessCalls,
	    Common::DebugCounters::Counter::DispatchProcessNs);
	EXIT_IF(buffer.IsInvalid());
	{
		Common::DebugCounters::StageTimer pop_stage(Common::DebugCounters::Counter::PopPendingNs);
		m_context.GetCommandScheduler().PopPendingOperations();
	}
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();

	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	input_info.workgroup_counts[0] = thread_group_x;
	input_info.workgroup_counts[1] = thread_group_y;
	input_info.workgroup_counts[2] = thread_group_z;
	if (use_thread_dimensions) {
		const uint32_t group_sizes[] = {cs_regs.cs_regs.num_thread_x, cs_regs.cs_regs.num_thread_y,
		                                cs_regs.cs_regs.num_thread_z};
		for (uint32_t axis = 0; axis < 3u; ++axis) {
			const auto size = std::max(group_sizes[axis], 1u);
			const auto threads = input_info.workgroup_counts[axis];
			input_info.workgroup_counts[axis] = threads / size + (threads % size != 0u);
		}
	}
	Common::DebugCounters::StageSequence stages(Common::DebugCounters::Counter::CsProgramNs);
	m_context.GetImageHeap().BeginDraw();
	static const bool log_drops = std::getenv("KYTY_CHECK_GRID_UPLOADS") != nullptr;
	GpuNoteReadbackReader(cs_regs.cs_regs.data_addr);
	if (!m_context.GetPipelineCache().GetComputeProgram(cs_regs, input_info)) {
		if (log_drops) {
			static std::atomic<uint32_t> logged {0};
			if (logged.fetch_add(1) < 200) {
				std::printf("dropped dispatch: no program, code=0x%016" PRIx64 " groups=%u,%u,%u\n",
				            static_cast<uint64_t>(cs_regs.cs_regs.data_addr), thread_group_x,
				            thread_group_y, thread_group_z);
			}
		}
		return;
	}
	GpuNoteCurrentDispatch({input_info.stage.module->info.shader_hash,
	                        {thread_group_x, thread_group_y, thread_group_z}, false});
	if (static const bool rb_trace = std::getenv("KYTY_RB_TRACE") != nullptr; rb_trace) {
		static std::unordered_set<uint64_t> seen;
		if (seen.size() < 2000 && seen.insert(cs_regs.cs_regs.data_addr).second) {
			std::printf("CODE 0x%" PRIx64 " = shader 0x%016" PRIx64 "\n",
			            static_cast<uint64_t>(cs_regs.cs_regs.data_addr),
			            input_info.stage.module->info.shader_hash);
		}
	}
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const auto& program   = input_info.stage.module->info;
	const auto& resources = *input_info.stage.resources;
	stages.Next(Common::DebugCounters::Counter::CsFastPathNs);
	if (resources.specialization_reads.empty()) {
		const bool meta = TryConsumeComputeMetaClear(input_info, buffer);
		if (meta || TryConsumeComputeFill(input_info, buffer, thread_group_x, thread_group_y,
		                                  thread_group_z, mode)) {
			if (meta && log_drops) {
				static std::atomic<uint32_t> logged {0};
				if (logged.fetch_add(1) < 200) {
					std::printf("consumed dispatch: meta clear shader=0x%016" PRIx64 " groups=%u,%u,%u\n",
					            program.shader_hash, thread_group_x, thread_group_y, thread_group_z);
				}
			}
			ResetBindings();
			return;
		}
	}

	if (use_thread_dimensions) {
		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = input_info.workgroup_counts[0];
		thread_group_y       = input_info.workgroup_counts[1];
		thread_group_z       = input_info.workgroup_counts[2];

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	stages.Next(Common::DebugCounters::Counter::CsPipelineNs);
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	if (program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) {
		std::copy(std::begin(input_info.dispatch_threads_num), std::end(input_info.dispatch_threads_num),
		          bindings.shader_data.begin() + program.bindings.dispatch_thread_dword);
	}
	PreparedBindings* descriptor_stage = &bindings;
	stages.Next(Common::DebugCounters::Counter::CsFindBuffersNs);
	// CPU-data buffer uploads for this dispatch share one barrier pair.
	std::optional<BufferCache::UploadBatchScope> upload_batch;
	upload_batch.emplace(m_context.GetBufferCache());
	FindBuffers(std::span {&descriptor_stage, 1u});
	stages.Next(Common::DebugCounters::Counter::CsBdaNs);
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	stages.Next(Common::DebugCounters::Counter::CsImageHeapNs);
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	stages.Next(Common::DebugCounters::Counter::CsImagesNs);
	RebindImages(bindings);
	stages.Next(Common::DebugCounters::Counter::CsBuffersNs);
	BindSharedMemory(m_context, input_info, bindings);
	RebindBuffers(bindings);
	upload_batch.reset();
	stages.Next(Common::DebugCounters::Counter::CsCommitNs);

	// LDS storage and GDS are shared by every dispatch, so those dispatches never overlap.
	// KYTY_CHAIN_SINGLE_GROUP=1 relaxes only runs of single-workgroup dispatches: NHL 27 issues
	// chains of 1-group solver dispatches (one per object) without guest sync in between, which
	// the guest GPU overlaps; serialized, each occupies one SM for ~0.7 ms.
	const bool single_group =
	    ChainSingleGroupDispatches() && Common::DebugCounters::AbVariant() &&
	    uint64_t {thread_group_x} * thread_group_y * thread_group_z == 1u &&
	    ChainShaderAllowed(program.shader_hash);
	const bool chainable = (RelaxedComputeBarriers() || single_group) && !async_compute &&
	                       bindings.shared_memory.buffer == nullptr && !program.info.uses_gds;
	const bool chained   = chainable && buffer.ComputeBarrierPending();
	// KYTY_CHAIN_CHECK=1: report descriptor ranges of a chained dispatch that overlap a range an
	// earlier member of the same chain wrote (or that it writes over an earlier member's reads).
	static const bool chain_check = std::getenv("KYTY_CHAIN_CHECK") != nullptr;
	if (chain_check && chainable) {
		struct Access { uint64_t address, size, hash; uint32_t position; bool written; };
		static std::vector<Access> members;
		static uint32_t            position = 0;
		static uint32_t            reported = 0;
		static uint32_t            chains   = 0;
		if (!chained) {
			members.clear();
			position = 0;
			chains++;
		}
		std::vector<Access> current;
		for (const auto& resource: program.info.buffers) {
			if (resource.descriptor_index == UINT32_MAX ||
			    resource.descriptor_index >= bindings.buffer_sources.size())
				continue;
			const auto& source = bindings.buffer_sources[resource.descriptor_index];
			if (source.address == 0 || source.size == 0) continue;
			current.push_back({source.address, source.size, program.shader_hash, position,
			                   resource.written});
		}
		for (const auto& now: current) {
			for (const auto& before: members) {
				if (!(now.written || before.written)) continue;
				if (now.address < before.address + before.size &&
				    before.address < now.address + now.size && reported < 60) {
					reported++;
					std::printf("CHAIN OVERLAP chain#%u: #%u 0x%016" PRIx64 " %s 0x%" PRIx64
					            "+0x%" PRIx64 " vs #%u 0x%016" PRIx64 " %s 0x%" PRIx64 "+0x%" PRIx64
					            " addr_writes=%d dma=%d\n",
					            chains, now.position, now.hash, now.written ? "W" : "R", now.address,
					            now.size, before.position, before.hash, before.written ? "W" : "R",
					            before.address, before.size, program.has_address_writes ? 1 : 0,
					            program.info.uses_dma ? 1 : 0);
					std::fflush(stdout);
				}
			}
		}
		static uint32_t summaries = 0;
		if (position < 8 && summaries < 20 && chains > 200 && chains % 50 == 0) {
			summaries++;
			std::printf("CHAIN member chain#%u #%u 0x%016" PRIx64 " buffers=%zu addr_writes=%d dma=%d\n",
			            chains, position, program.shader_hash, current.size(),
			            program.has_address_writes ? 1 : 0, program.info.uses_dma ? 1 : 0);
			for (const auto& a: current)
				std::printf("   %s 0x%" PRIx64 "+0x%" PRIx64 "\n", a.written ? "W" : "R", a.address, a.size);
			std::fflush(stdout);
		}
		members.insert(members.end(), current.begin(), current.end());
		position++;
	}
	std::optional<CommandBuffer::DispatchChainScope> chain;
	if (chainable) {
		chain.emplace(buffer);
	}
	auto              vk_buffer        = buffer.Handle();
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	stages.Next(Common::DebugCounters::Counter::CsRecordNs);
	LogUnalignedBufferBases(input_info.stage, program.shader_hash);
	bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	if (has_storage_writes) {
		if (chained && buffer.OnlyDispatchesSinceHazard()) {
			// Only guest dispatches ran since the last hazard barrier, and their ordering is
			// the guest's own synchronization.
			Common::DebugCounters::Add(Common::DebugCounters::Counter::CsHazardsElided);
		} else {
			// A host fence used to serialize every dispatch. Preserve its read-before-write
			// ordering while allowing the queue to execute asynchronously.
			ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
			buffer.NoteHazardBarrier();
		}
	}
	if (Config::GraphicsDebugDumpEnabled()) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " tick=%" PRIu64 " completed_tick=%" PRIu64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, program.shader_hash,
		     m_context.GetCommandScheduler().CurrentTick(),
		     m_context.GetCommandScheduler().GetMasterSemaphore().KnownGpuTick(),
		     thread_group_x, thread_group_y, thread_group_z, mode,
		     input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(
			    resources.samplers[program.info.samplers[i].snapshot_index]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	GpuProfileNoteAsyncCompute(async_compute);
	GpuTraceDispatch(program.shader_hash, thread_group_x, thread_group_y, thread_group_z);
	auto [written_base, read_base] = GpuCheckpointsEnabled() ? FirstBufferBases(input_info.stage)
	                                                         : std::pair<uint64_t, uint64_t> {};
	if (GpuCheckpointsEnabled()) {
		// Image writes take precedence: device-loss reports trace image producers.
		if (const auto image = FirstImageAddress(*input_info.stage.module, bindings, true);
		    image != 0) {
			written_base = image;
		}
		if (read_base == 0) {
			read_base = FirstImageAddress(*input_info.stage.module, bindings, false);
		}
	}
	const auto checkpoint = GpuCheckpointBegin(vk_buffer, GpuCheckpointOp::Dispatch,
	                                           program.shader_hash, thread_group_x,
	                                           thread_group_y, thread_group_z, written_base,
	                                           read_base, async_compute);
	constexpr uint32_t DISPATCH_INITIATOR_ORDERED_APPEND_ENABLE = 1u << 3u;
	const uint64_t     group_threads = uint64_t {std::max(input_info.threads_num[0], 1u)} *
	                                   std::max(input_info.threads_num[1], 1u) *
	                                   std::max(input_info.threads_num[2], 1u);
	if ((mode & DISPATCH_INITIATOR_ORDERED_APPEND_ENABLE) != 0 && program.info.uses_gds &&
	    group_threads <= input_info.wave_size && !input_info.lds_storage) {
		// A single guest wave per group can run in launch order without a device-wide
		// spinlock. Keep the guest workgroup IDs when splitting the dispatch.
		for (uint32_t z = 0; z < thread_group_z; ++z) {
			for (uint32_t y = 0; y < thread_group_y; ++y) {
				for (uint32_t x = 0; x < thread_group_x; ++x) {
					if ((x | y | z) != 0u) {
						ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
					}
					vk_buffer.dispatchBase(x, y, z, 1, 1, 1);
				}
			}
		}
	} else {
		vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);
	}
	GpuCheckpointEnd(vk_buffer, checkpoint);
	if (buffer.RawHandle() != vk_buffer) {
		ReportRetiredRecording("dispatch", program.shader_hash);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);

	if (chainable) {
		buffer.DeferComputeBarrier();
		Common::DebugCounters::Add(Common::DebugCounters::Counter::CsBarriersDeferred);
		if (chained) {
			Common::DebugCounters::Add(Common::DebugCounters::Counter::CsBarriersElided);
		}
	} else {
		// The removed host fence also ordered read-only dispatches before later writers.
		ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	ResetBindings();
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode, bool async_compute) {
	Common::DebugCounters::ScopedTimer timer(
	    Common::DebugCounters::Counter::DispatchProcessCalls,
	    Common::DebugCounters::Counter::DispatchProcessNs);
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	vk::DispatchIndirectCommand args {};
	if (!m_context.GetBufferCache().HasGpuDirtyBytes(args_addr, sizeof(args)) &&
	    LibKernel::Memory::TryReadBacking(args_addr, &args, sizeof(args)) &&
	    (args.x == 0 || args.y == 0 || args.z == 0)) {
		return;
	}
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	input_info.indirect_args_address = args_addr;
	m_context.GetImageHeap().BeginDraw();
	GpuNoteReadbackReader(cs_regs.cs_regs.data_addr | (uint64_t {1} << 63u));
	if (!m_context.GetPipelineCache().GetComputeProgram(cs_regs, input_info)) return;
	GpuNoteCurrentDispatch({input_info.stage.module->info.shader_hash, {0, 0, 0}, true});
	if (static const bool rb_trace = std::getenv("KYTY_RB_TRACE") != nullptr; rb_trace) {
		static std::unordered_set<uint64_t> seen;
		if (seen.size() < 2000 && seen.insert(cs_regs.cs_regs.data_addr).second) {
			std::printf("CODE 0x%" PRIx64 " = shader 0x%016" PRIx64 " (indirect)\n",
			            static_cast<uint64_t>(cs_regs.cs_regs.data_addr),
			            input_info.stage.module->info.shader_hash);
		}
	}
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	PreparedBindings* descriptor_stage = &bindings;
	FindBuffers(std::span {&descriptor_stage, 1u});
	const auto& program = input_info.stage.module->info;
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	BindSharedMemory(m_context, input_info, bindings, args_addr);
	if (program.info.uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const auto vk_buffer = buffer.Handle();
	const auto tick_at_capture = m_context.GetCommandScheduler().CurrentTick();
	const bool submit_probe = GpuIndirectArgsLogEnabled();
	if (submit_probe) g_submit_probe = 1;
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (submit_probe) g_submit_probe = 2;
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	if (submit_probe) g_submit_probe = 3;
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eDrawIndirect, {},
	                          1, &barrier, 0, nullptr, 0, nullptr);
	// KYTY_TRACE_IMAGE_READS=hash: print the bound images of that shader's indirect dispatches
	// (first 300), to see where its per-frame inputs come from.
	static const uint64_t trace_reads = [] {
		const char* value = std::getenv("KYTY_TRACE_IMAGE_READS");
		return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ull;
	}();
	if (trace_reads != 0 && program.shader_hash == trace_reads) {
		static std::atomic<uint32_t> traced {0};
		if (traced.fetch_add(1) < 300) {
			auto& cache = m_context.GetTextureCache();
			for (size_t i = 0; i < bindings.images.size(); i++) {
				const auto& binding = bindings.images[i];
				if (!binding.image_id) {
					continue;
				}
				auto& image = cache.GetImage(binding.image_id);
				std::printf("image read 0x%016" PRIx64 " #%zu: guest=0x%016" PRIx64
				            " %ux%u format=%d id=%u gpu_modified=%d buffer_modified=%d "
				            "cpu_dirty=%d render_target=%d\n",
				            program.shader_hash, i, binding.desc.info.data.address,
				            binding.desc.info.extent.width, binding.desc.info.extent.height,
				            static_cast<int>(image.backing.format),
				            static_cast<uint32_t>(binding.image_id.index), image.IsGpuModified() ? 1 : 0,
				            image.IsBufferModified() ? 1 : 0, image.IsCpuDirty() ? 1 : 0,
				            image.usage.render_target ? 1 : 0);
			}
		}
	}
	// KYTY_LOG_INDIRECT_ONLY=hash[,hash...]: limit the indirect-args ring to these dispatches.
	static const std::vector<uint64_t> log_only = [] {
		std::vector<uint64_t> list;
		const char*           value = std::getenv("KYTY_LOG_INDIRECT_ONLY");
		for (const char* p = value; p != nullptr && *p != '\0';) {
			char*          end  = nullptr;
			const uint64_t hash = std::strtoull(p, &end, 16);
			if (end == p) break;
			list.push_back(hash);
			p = *end == ',' ? end + 1 : end;
		}
		return list;
	}();
	const bool log_this = log_only.empty() ||
	                      std::find(log_only.begin(), log_only.end(), program.shader_hash) != log_only.end();
	if (!log_only.empty() && log_this) {
		static std::mutex             mutex;
		static std::set<uint64_t>     seen;
		std::scoped_lock              lock(mutex);
		if (seen.insert(program.shader_hash).second) {
			std::printf("indirect dispatch 0x%016" PRIx64 " async_compute=%d gpu_thread=%d\n",
			            program.shader_hash, async_compute ? 1 : 0, GuestGpu::IsGpuThread() ? 1 : 0);
		}
	}
	if (submit_probe) g_submit_probe = 5;
	if (GpuIndirectArgsLogEnabled() && log_this) {
		GpuIndirectArgsHostState host {};
		auto&                    cache = m_context.GetBufferCache();
		host.gpu_dirty                 = cache.IsRegionGpuModified(args_addr, 12);
		host.cpu_dirty                 = cache.IsRegionCpuModified(args_addr, 12);
		// Reading GPU-modified backing downloads it, which submits the command buffer this
		// dispatch is being recorded into; skip it so the log cannot drop the dispatch.
		host.backing_valid = !host.gpu_dirty &&
		                     Libs::LibKernel::Memory::TryReadBufferBacking(args_addr, host.backing, 12);
		host.count  = static_cast<uint32_t>(program.shader_hash); // identifies the dispatch
		host.stride = 0xd15;
		host.source_size = args_buffer->Size();
		host.tick        = m_context.GetCommandScheduler().CurrentTick();
		GpuLogIndirectArgs(vk_buffer, args_buffer->Handle(), args_offset, args_addr, 12, host);
	}
	if (submit_probe) g_submit_probe = 6;
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	if (IndirectDiagnosticsEnabled()) {
		LogIndirectDispatch(m_context, program.shader_hash, args_addr, input_info.threads_num);
	}
	GpuProfileNoteAsyncCompute(async_compute);
	const auto checkpoint = GpuCheckpointBegin(
	    vk_buffer, GpuCheckpointOp::DispatchIndirect, program.shader_hash, 0, 0, 0, args_addr,
	    GpuCheckpointsEnabled() ? FirstImageAddress(*input_info.stage.module, bindings, false) : 0,
	    async_compute);
	vk_buffer.dispatchIndirect(args_buffer->Handle(), args_offset);
	GpuCheckpointEnd(vk_buffer, checkpoint);
	if (submit_probe) g_submit_probe = 0;
	if (buffer.RawHandle() != vk_buffer) {
		ReportRetiredRecording("indirect dispatch", program.shader_hash);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
	ShaderAccessBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	ResetBindings();
}

} // namespace Libs::Graphics
