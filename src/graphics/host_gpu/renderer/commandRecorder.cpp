#include "graphics/host_gpu/renderer/commandRecorder.h"

#include "common/debugCounters.h"

#include <algorithm>

namespace Libs::Graphics {

CommandRecorder::CommandRecorder(BeginCallback begin, std::function<uint64_t()> current_tick)
    : m_begin(std::move(begin)), m_current_tick(std::move(current_tick)),
      m_worker([this](std::stop_token stop) { Run(stop); }) {}

CommandRecorder::~CommandRecorder() {
	WaitIdle();
	m_worker.request_stop();
	m_available.notify_all();
	if (m_worker.joinable()) {
		m_worker.join();
	}
	if (m_current != nullptr) {
		Recycle(m_current);
	}
	for (auto* chunk: m_free) {
		delete chunk;
	}
}

void* CommandRecorder::Allocate(size_t size, size_t alignment) {
	const auto fits = [&](const Chunk* chunk) {
		const auto offset = (chunk->used + alignment - 1) & ~(alignment - 1);
		return offset + size <= chunk->bytes.size();
	};
	if (m_current == nullptr || !fits(m_current)) {
		if (m_current != nullptr) {
			FlushChunk();
		}
		{
			std::unique_lock lock(m_mutex);
			// Back-pressure: the worker may lag the GPU thread by a bounded amount only.
			m_idle.wait(lock, [this] { return m_jobs.size() < MaxQueued; });
			if (!m_free.empty() && m_free.back()->bytes.size() >= size + alignment) {
				m_current = m_free.back();
				m_free.pop_back();
			}
		}
		if (m_current == nullptr) {
			m_current = new Chunk;
			m_current->bytes.resize(std::max(ChunkSize, size + alignment));
		}
		m_current->tick = m_current_tick();
	}
	const auto offset = (m_current->used + alignment - 1) & ~(alignment - 1);
	m_current->used   = offset + size;
	return m_current->bytes.data() + offset;
}

void CommandRecorder::FlushChunk() {
	if (m_current == nullptr) {
		return;
	}
	auto* chunk = m_current;
	m_current   = nullptr;
	Push({chunk, {}, chunk->tick});
}

void CommandRecorder::Push(Job&& job) {
	{
		std::lock_guard lock(m_mutex);
		m_jobs.push_back(std::move(job));
	}
	m_available.notify_one();
}

void CommandRecorder::Submit(SubmitCallback submit) {
	const auto tick = m_current_tick();
	FlushChunk();
	Push({nullptr, std::move(submit), tick});
}

void CommandRecorder::WaitIdle() {
	FlushChunk();
	std::unique_lock lock(m_mutex);
	m_idle.wait(lock, [this] { return m_jobs.empty() && !m_busy; });
}

void CommandRecorder::Recycle(Chunk* chunk) {
	for (auto* command = chunk->first; command != nullptr;) {
		auto* next = command->next;
		command->~Command();
		command = next;
	}
	chunk->first = chunk->last = nullptr;
	chunk->used                = 0;
	if (chunk->bytes.size() > ChunkSize) {
		delete chunk;
		return;
	}
	std::lock_guard lock(m_mutex);
	m_free.push_back(chunk);
}

void CommandRecorder::Run(std::stop_token stop) {
	vk::CommandBuffer   buffer;
	std::vector<Chunk*> batch;
	for (;;) {
		Job job;
		{
			std::unique_lock lock(m_mutex);
			m_busy = false;
			m_idle.notify_all();
			m_available.wait(lock, [&] { return stop.stop_requested() || !m_jobs.empty(); });
			if (m_jobs.empty()) {
				return;
			}
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
			m_busy = true;
		}
		m_idle.notify_all();
		if (!buffer) {
			buffer = m_begin(job.tick);
		}
		if (job.chunk != nullptr) {
			for (auto* command = job.chunk->first; command != nullptr; command = command->next) {
				command->Execute(buffer);
			}
			// Later commands of this buffer may point into this chunk's copied arrays.
			batch.push_back(job.chunk);
		} else {
			job.submit(buffer);
			buffer = nullptr;
			for (auto* chunk: batch) {
				Recycle(chunk);
			}
			batch.clear();
		}
	}
}

// --- Commands with pointer arguments: copy what they point to. ---

void CmdRef::pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
                             vk::DependencyFlags flags, uint32_t memory_count,
                             const vk::MemoryBarrier* memory, uint32_t buffer_count,
                             const vk::BufferMemoryBarrier* buffers, uint32_t image_count,
                             const vk::ImageMemoryBarrier* images) const {
	if (m_recorder == nullptr) {
		m_direct.pipelineBarrier(src, dst, flags, memory_count, memory, buffer_count, buffers,
		                         image_count, images);
		return;
	}
	const auto* m = m_recorder->Copy(memory, memory_count);
	const auto* b = m_recorder->Copy(buffers, buffer_count);
	const auto* i = m_recorder->Copy(images, image_count);
	m_recorder->Record([=](vk::CommandBuffer c) {
		c.pipelineBarrier(src, dst, flags, memory_count, m, buffer_count, b, image_count, i);
	});
}

void CmdRef::pipelineBarrier2(const vk::DependencyInfo& dependency) const {
	if (m_recorder == nullptr) {
		m_direct.pipelineBarrier2(dependency);
		return;
	}
	EXIT_IF(dependency.pNext != nullptr);
	auto copy = dependency;
	copy.pMemoryBarriers = m_recorder->Copy(dependency.pMemoryBarriers, dependency.memoryBarrierCount);
	copy.pBufferMemoryBarriers =
	    m_recorder->Copy(dependency.pBufferMemoryBarriers, dependency.bufferMemoryBarrierCount);
	copy.pImageMemoryBarriers =
	    m_recorder->Copy(dependency.pImageMemoryBarriers, dependency.imageMemoryBarrierCount);
	m_recorder->Record([copy](vk::CommandBuffer c) { c.pipelineBarrier2(copy); });
}

void CmdRef::copyBuffer(vk::Buffer src, vk::Buffer dst, uint32_t count,
                        const vk::BufferCopy* regions) const {
	if (m_recorder == nullptr) {
		m_direct.copyBuffer(src, dst, count, regions);
		return;
	}
	const auto* r = m_recorder->Copy(regions, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.copyBuffer(src, dst, count, r); });
}

void CmdRef::copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                       vk::ImageLayout dst_layout, uint32_t count, const vk::ImageCopy* regions) const {
	if (m_recorder == nullptr) {
		m_direct.copyImage(src, src_layout, dst, dst_layout, count, regions);
		return;
	}
	const auto* r = m_recorder->Copy(regions, count);
	m_recorder->Record(
	    [=](vk::CommandBuffer c) { c.copyImage(src, src_layout, dst, dst_layout, count, r); });
}

void CmdRef::copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout,
                               uint32_t count, const vk::BufferImageCopy* regions) const {
	if (m_recorder == nullptr) {
		m_direct.copyBufferToImage(src, dst, layout, count, regions);
		return;
	}
	const auto* r = m_recorder->Copy(regions, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.copyBufferToImage(src, dst, layout, count, r); });
}

void CmdRef::copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst,
                               uint32_t count, const vk::BufferImageCopy* regions) const {
	if (m_recorder == nullptr) {
		m_direct.copyImageToBuffer(src, layout, dst, count, regions);
		return;
	}
	const auto* r = m_recorder->Copy(regions, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.copyImageToBuffer(src, layout, dst, count, r); });
}

void CmdRef::resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                          vk::ImageLayout dst_layout, uint32_t count,
                          const vk::ImageResolve* regions) const {
	if (m_recorder == nullptr) {
		m_direct.resolveImage(src, src_layout, dst, dst_layout, count, regions);
		return;
	}
	const auto* r = m_recorder->Copy(regions, count);
	m_recorder->Record(
	    [=](vk::CommandBuffer c) { c.resolveImage(src, src_layout, dst, dst_layout, count, r); });
}

void CmdRef::blitImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                       vk::ImageLayout dst_layout, uint32_t count, const vk::ImageBlit* regions,
                       vk::Filter filter) const {
	if (m_recorder == nullptr) {
		m_direct.blitImage(src, src_layout, dst, dst_layout, count, regions, filter);
		return;
	}
	const auto* r = m_recorder->Copy(regions, count);
	m_recorder->Record([=](vk::CommandBuffer c) {
		c.blitImage(src, src_layout, dst, dst_layout, count, r, filter);
	});
}

void CmdRef::clearColorImage(vk::Image image, vk::ImageLayout layout,
                             const vk::ClearColorValue* color, uint32_t count,
                             const vk::ImageSubresourceRange* ranges) const {
	if (m_recorder == nullptr) {
		m_direct.clearColorImage(image, layout, color, count, ranges);
		return;
	}
	const auto value = *color;
	const auto* r    = m_recorder->Copy(ranges, count);
	m_recorder->Record(
	    [=](vk::CommandBuffer c) { c.clearColorImage(image, layout, &value, count, r); });
}

void CmdRef::clearDepthStencilImage(vk::Image image, vk::ImageLayout layout,
                                    const vk::ClearDepthStencilValue* value, uint32_t count,
                                    const vk::ImageSubresourceRange* ranges) const {
	if (m_recorder == nullptr) {
		m_direct.clearDepthStencilImage(image, layout, value, count, ranges);
		return;
	}
	const auto clear = *value;
	const auto* r    = m_recorder->Copy(ranges, count);
	m_recorder->Record(
	    [=](vk::CommandBuffer c) { c.clearDepthStencilImage(image, layout, &clear, count, r); });
}

void CmdRef::clearAttachments(uint32_t count, const vk::ClearAttachment* attachments,
                              uint32_t rect_count, const vk::ClearRect* rects) const {
	if (m_recorder == nullptr) {
		m_direct.clearAttachments(count, attachments, rect_count, rects);
		return;
	}
	const auto* a = m_recorder->Copy(attachments, count);
	const auto* r = m_recorder->Copy(rects, rect_count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.clearAttachments(count, a, rect_count, r); });
}

void CmdRef::pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                                  uint32_t set, uint32_t count,
                                  const vk::WriteDescriptorSet* writes) const {
	if (m_recorder == nullptr) {
		m_direct.pushDescriptorSetKHR(bind_point, layout, set, count, writes);
		return;
	}
	auto* w = m_recorder->CopyMutable(writes, count);
	for (uint32_t index = 0; index < count; ++index) {
		auto& write = w[index];
		EXIT_IF(write.pNext != nullptr);
		write.pImageInfo       = m_recorder->Copy(write.pImageInfo, write.descriptorCount);
		write.pBufferInfo      = m_recorder->Copy(write.pBufferInfo, write.descriptorCount);
		write.pTexelBufferView = m_recorder->Copy(write.pTexelBufferView, write.descriptorCount);
	}
	m_recorder->Record(
	    [=](vk::CommandBuffer c) { c.pushDescriptorSetKHR(bind_point, layout, set, count, w); });
}

void CmdRef::bindDescriptorSets(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                                uint32_t first, uint32_t count, const vk::DescriptorSet* sets,
                                uint32_t dynamic_count, const uint32_t* dynamic_offsets) const {
	if (m_recorder == nullptr) {
		m_direct.bindDescriptorSets(bind_point, layout, first, count, sets, dynamic_count,
		                            dynamic_offsets);
		return;
	}
	const auto* s = m_recorder->Copy(sets, count);
	const auto* d = m_recorder->Copy(dynamic_offsets, dynamic_count);
	m_recorder->Record([=](vk::CommandBuffer c) {
		c.bindDescriptorSets(bind_point, layout, first, count, s, dynamic_count, d);
	});
}

void CmdRef::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset,
                           uint32_t size, const void* values) const {
	if (m_recorder == nullptr) {
		m_direct.pushConstants(layout, stages, offset, size, values);
		return;
	}
	const auto* v = m_recorder->Copy(static_cast<const std::byte*>(values), size);
	m_recorder->Record(
	    [=](vk::CommandBuffer c) { c.pushConstants(layout, stages, offset, size, v); });
}

void CmdRef::beginRendering(const vk::RenderingInfo& info) const {
	if (m_recorder == nullptr) {
		m_direct.beginRendering(info);
		return;
	}
	EXIT_IF(info.pNext != nullptr);
	auto copy              = info;
	copy.pColorAttachments = m_recorder->Copy(info.pColorAttachments, info.colorAttachmentCount);
	copy.pDepthAttachment   = m_recorder->Copy(info.pDepthAttachment, 1);
	copy.pStencilAttachment = m_recorder->Copy(info.pStencilAttachment, 1);
	m_recorder->Record([copy](vk::CommandBuffer c) { c.beginRendering(copy); });
}

void CmdRef::bindVertexBuffers2(uint32_t first, uint32_t count, const vk::Buffer* buffers,
                                const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                                const vk::DeviceSize* strides) const {
	if (m_recorder == nullptr) {
		m_direct.bindVertexBuffers2(first, count, buffers, offsets, sizes, strides);
		return;
	}
	const auto* b = m_recorder->Copy(buffers, count);
	const auto* o = m_recorder->Copy(offsets, count);
	const auto* s = m_recorder->Copy(sizes, count);
	const auto* t = m_recorder->Copy(strides, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.bindVertexBuffers2(first, count, b, o, s, t); });
}

void CmdRef::setViewport(uint32_t first, uint32_t count, const vk::Viewport* viewports) const {
	if (m_recorder == nullptr) {
		m_direct.setViewport(first, count, viewports);
		return;
	}
	const auto* v = m_recorder->Copy(viewports, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.setViewport(first, count, v); });
}

void CmdRef::setScissor(uint32_t first, uint32_t count, const vk::Rect2D* rects) const {
	if (m_recorder == nullptr) {
		m_direct.setScissor(first, count, rects);
		return;
	}
	const auto* r = m_recorder->Copy(rects, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.setScissor(first, count, r); });
}

void CmdRef::setViewportWithCount(uint32_t count, const vk::Viewport* viewports) const {
	if (m_recorder == nullptr) {
		m_direct.setViewportWithCount(count, viewports);
		return;
	}
	const auto* v = m_recorder->Copy(viewports, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.setViewportWithCount(count, v); });
}

void CmdRef::setScissorWithCount(uint32_t count, const vk::Rect2D* rects) const {
	if (m_recorder == nullptr) {
		m_direct.setScissorWithCount(count, rects);
		return;
	}
	const auto* r = m_recorder->Copy(rects, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.setScissorWithCount(count, r); });
}

void CmdRef::setColorWriteEnableEXT(uint32_t count, const vk::Bool32* enables) const {
	if (m_recorder == nullptr) {
		m_direct.setColorWriteEnableEXT(count, enables);
		return;
	}
	const auto* e = m_recorder->Copy(enables, count);
	m_recorder->Record([=](vk::CommandBuffer c) { c.setColorWriteEnableEXT(count, e); });
}

void CmdRef::setBlendConstants(const float constants[4]) const {
	if (m_recorder == nullptr) {
		m_direct.setBlendConstants(constants);
		return;
	}
	std::array<float, 4> value {constants[0], constants[1], constants[2], constants[3]};
	m_recorder->Record([value](vk::CommandBuffer c) { c.setBlendConstants(value.data()); });
}

} // namespace Libs::Graphics
