#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderBindings.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

vk::ShaderStageFlagBits NativeShaderStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Local:
		case ShaderType::Vertex: return vk::ShaderStageFlagBits::eVertex;
		case ShaderType::Mesh: return vk::ShaderStageFlagBits::eMeshEXT;
		case ShaderType::TessellationControl: return vk::ShaderStageFlagBits::eTessellationControl;
		case ShaderType::TessellationEvaluation:
			return vk::ShaderStageFlagBits::eTessellationEvaluation;
		case ShaderType::Pixel: return vk::ShaderStageFlagBits::eFragment;
		case ShaderType::Compute: return vk::ShaderStageFlagBits::eCompute;
		default: EXIT("unknown native shader stage\n");
	}
}

vk::PipelineStageFlags ShaderPipelineStages(vk::ShaderStageFlags stages) {
	vk::PipelineStageFlags result = {};
	if (stages & vk::ShaderStageFlagBits::eVertex) {
		result |= vk::PipelineStageFlagBits::eVertexShader;
	}
	if (stages & vk::ShaderStageFlagBits::eMeshEXT) {
		result |= vk::PipelineStageFlagBits::eMeshShaderEXT;
	}
	if (stages & vk::ShaderStageFlagBits::eTessellationControl) {
		result |= vk::PipelineStageFlagBits::eTessellationControlShader;
	}
	if (stages & vk::ShaderStageFlagBits::eTessellationEvaluation) {
		result |= vk::PipelineStageFlagBits::eTessellationEvaluationShader;
	}
	if (stages & vk::ShaderStageFlagBits::eFragment) {
		result |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (stages & vk::ShaderStageFlagBits::eCompute) {
		result |= vk::PipelineStageFlagBits::eComputeShader;
	}
	EXIT_IF(!result);
	return result;
}

vk::MemoryBarrier MakeShaderWriteDependency() {
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
	                        vk::AccessFlagBits::eVertexAttributeRead |
	                        vk::AccessFlagBits::eIndexRead | vk::AccessFlagBits::eUniformRead |
	                        vk::AccessFlagBits::eIndirectCommandRead |
	                        vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eColorAttachmentRead |
	                        vk::AccessFlagBits::eColorAttachmentWrite;
	return barrier;
}

vk::MemoryBarrier MakeShaderAccessDependency() {
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	return barrier;
}

vk::MemoryBarrier MakeShaderWriteHazardDependency() {
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	return barrier;
}

vk::BufferMemoryBarrier MakeGdsDependency(vk::Buffer buffer) {
	EXIT_IF(buffer == nullptr);

	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer;
	barrier.offset              = 0;
	barrier.size                = VK_WHOLE_SIZE;
	return barrier;
}

void LogUnalignedBufferBases(const ShaderStageRuntime& runtime, uint64_t shader_hash) {
	// Descriptor-bound 32-bit buffer accesses index whole dwords, so a guest base that is not
	// 4-byte aligned would read/write shifted data (the guest GPU allows unaligned access).
	// KYTY_LOG_UNALIGNED=1 enables this diagnostic (unaligned access is handled since 2026-11-05).
	static const bool enabled = std::getenv("KYTY_LOG_UNALIGNED") != nullptr;
	static std::atomic<uint32_t> logged {0};
	if (!enabled || !runtime || logged.load(std::memory_order_relaxed) >= 200) {
		return;
	}
	const auto& program   = runtime.module->info;
	const auto& resources = *runtime.resources;
	for (uint32_t i = 0; i < program.info.buffers.size() && i < resources.buffers.size(); i++) {
		const auto& value = resources.buffers[i];
		if (value.dword_count < 4) {
			continue;
		}
		ShaderBufferResource descriptor;
		std::memcpy(descriptor.fields, value.dwords.data(), sizeof(descriptor.fields));
		const auto base = descriptor.Base48();
		if (base == 0 || (base & 3u) == 0 || descriptor.NumRecords() == 0) {
			continue;
		}
		if (logged.fetch_add(1, std::memory_order_relaxed) < 200) {
			std::printf("unaligned buffer base: shader=0x%016" PRIx64 " buffer=%u base=0x%016" PRIx64
			            " records=%u stride=%u written=%d\n",
			            shader_hash, i, base, descriptor.NumRecords(), descriptor.Stride(),
			            program.info.buffers[i].written ? 1 : 0);
		}
	}
}

std::pair<uint64_t, uint64_t> FirstBufferBases(const ShaderStageRuntime& runtime) {
	std::pair<uint64_t, uint64_t> bases {};
	if (!runtime) {
		return bases;
	}
	const auto& program   = runtime.module->info;
	const auto& resources = *runtime.resources;
	for (uint32_t i = 0; i < program.info.buffers.size() && i < resources.buffers.size(); i++) {
		const auto& value = resources.buffers[i];
		if (value.dword_count < 4) {
			continue;
		}
		ShaderBufferResource descriptor;
		std::memcpy(descriptor.fields, value.dwords.data(), sizeof(descriptor.fields));
		auto& slot = program.info.buffers[i].written ? bases.first : bases.second;
		if (slot == 0) {
			slot = descriptor.Base48();
		}
	}
	return bases;
}

bool HasShaderBufferWrites(const ShaderStageRuntime& runtime) {
	EXIT_IF(!runtime);
	const auto& program   = runtime.module->info;
	const auto& resources = *runtime.resources;
	EXIT_IF(resources.buffers.size() != program.info.buffers.size());
	// Address writes reach memory outside the bound buffer descriptors.
	bool has_writes = program.has_address_writes;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (!program.info.buffers[i].written) {
			continue;
		}
		const auto& value = resources.buffers[i];
		EXIT_IF(value.dword_count < 4);
		ShaderBufferResource descriptor;
		std::memcpy(descriptor.fields, value.dwords.data(), sizeof(descriptor.fields));
		// A zero stride means byte addressing. For either addressing mode a nonzero record
		// count is exactly the condition for a nonempty descriptor range.
		has_writes |= descriptor.Base48() != 0 && descriptor.NumRecords() != 0;
	}
	return has_writes;
}

void ShaderAccessBarrier(CmdRef vk_buffer, vk::PipelineStageFlags source_stages) {
	EXIT_IF(!vk_buffer || !source_stages);
	const auto barrier = MakeShaderAccessDependency();
	vk_buffer.pipelineBarrier(source_stages, vk::PipelineStageFlagBits::eAllCommands,
	                          vk::DependencyFlags {}, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ShaderWriteHazardBarrier(CmdRef      vk_buffer,
                              vk::PipelineStageFlags destination_stages) {
	EXIT_IF(!vk_buffer || !destination_stages);
	const auto barrier = MakeShaderWriteHazardDependency();
	vk_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, destination_stages,
	                          vk::DependencyFlags {}, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ShaderWriteBarrier(CmdRef vk_buffer, vk::PipelineStageFlags source_stages) {
	EXIT_IF(!vk_buffer || !source_stages);
	const auto barrier = MakeShaderWriteDependency();
	vk_buffer.pipelineBarrier(source_stages,
	                          vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          vk::DependencyFlags {}, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace Libs::Graphics
