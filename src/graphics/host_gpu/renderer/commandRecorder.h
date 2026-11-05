#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDRECORDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDRECORDER_H_

#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// Asynchronous Vulkan command recording (KYTY_ASYNC_RECORD). The GPU thread appends commands to
// chunks as closures; a worker thread replays them into the real VkCommandBuffer and submits it.
// Arrays the commands point to are copied into the chunk, so callers may reuse their storage
// right after the call. Chunks are recycled once their whole command buffer has been submitted.
class CommandRecorder {
public:
	struct Command {
		Command*     next = nullptr;
		virtual void Execute(vk::CommandBuffer command) = 0;
		virtual ~Command()                              = default;
	};

	struct Chunk {
		std::vector<std::byte> bytes;
		size_t                 used  = 0;
		Command*               first = nullptr;
		Command*               last  = nullptr;
		uint64_t               tick  = 0;
	};

	// A command buffer's end: the worker ends and submits it with this callback.
	using SubmitCallback = std::function<void(vk::CommandBuffer)>;
	// Returns a begun command buffer for the given scheduler tick (worker thread).
	using BeginCallback = std::function<vk::CommandBuffer(uint64_t tick)>;

	CommandRecorder(BeginCallback begin, std::function<uint64_t()> current_tick);
	~CommandRecorder();
	KYTY_CLASS_NO_COPY(CommandRecorder);

	template <typename F>
	void Record(F&& function) {
		using Type = Holder<std::decay_t<F>>;
		void* memory = Allocate(sizeof(Type), alignof(Type));
		auto* command = new (memory) Type(std::forward<F>(function));
		if (m_current->last != nullptr) {
			m_current->last->next = command;
		} else {
			m_current->first = command;
		}
		m_current->last = command;
	}

	// Copies count elements into the current chunk (nullptr stays nullptr).
	template <typename T>
	const T* Copy(const T* data, size_t count) {
		if (data == nullptr || count == 0) {
			return data;
		}
		static_assert(std::is_trivially_copyable_v<T>);
		auto* target = static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
		std::memcpy(target, data, sizeof(T) * count);
		return target;
	}
	template <typename T>
	T* CopyMutable(const T* data, size_t count) {
		return const_cast<T*>(Copy(data, count));
	}

	// Ends the current command buffer: the worker submits it through `submit` after replaying
	// everything recorded so far.
	void Submit(SubmitCallback submit);
	// Blocks until the worker has replayed and submitted everything queued.
	void WaitIdle();

private:
	template <typename F>
	struct Holder final : Command {
		explicit Holder(F&& f): function(std::move(f)) {}
		explicit Holder(const F& f): function(f) {}
		void Execute(vk::CommandBuffer command) override { function(command); }
		F    function;
	};

	struct Job {
		Chunk*         chunk = nullptr;
		SubmitCallback submit;
		uint64_t       tick = 0;
	};

	// Small chunks hand work to the worker every ~15 draws, so it replays while the GPU thread
	// keeps preparing (a whole command buffer fits in 256 KiB, which serialized the two).
	static constexpr size_t ChunkSize    = 16 * 1024;
	static constexpr size_t MaxQueued    = 4096;

	void* Allocate(size_t size, size_t alignment);
	void  FlushChunk();
	void  Push(Job&& job);
	void  Run(std::stop_token stop);
	void  Recycle(Chunk* chunk);

	BeginCallback             m_begin;
	std::function<uint64_t()> m_current_tick;
	Chunk*                    m_current = nullptr;

	std::mutex              m_mutex;
	std::condition_variable m_available;
	std::condition_variable m_idle;
	std::deque<Job>         m_jobs;
	std::vector<Chunk*>     m_free;
	bool                    m_busy = false;
	std::jthread            m_worker;
};

// What CommandBuffer::Handle() returns: the subset of vk::CommandBuffer the renderer uses.
// Without a recorder it calls the Vulkan buffer directly (immediate mode).
class CmdRef {
public:
	CmdRef() = default;
	// Immediate recording into a Vulkan buffer this thread owns.
	CmdRef(vk::CommandBuffer direct) noexcept: m_direct(direct) {} // NOLINT(google-explicit-constructor)
	CmdRef(CommandRecorder* recorder, vk::CommandBuffer direct, uint64_t epoch) noexcept
	    : m_recorder(recorder), m_direct(direct), m_epoch(epoch) {}

	bool operator==(const CmdRef& other) const noexcept = default;
	explicit operator bool() const noexcept { return m_recorder != nullptr || m_direct; }
	[[nodiscard]] bool     IsAsync() const noexcept { return m_recorder != nullptr; }
	// Identifies the command buffer being recorded (diagnostics only).
	[[nodiscard]] uint64_t Id() const noexcept {
		return m_recorder != nullptr ? m_epoch
		                             : reinterpret_cast<uint64_t>(static_cast<VkCommandBuffer>(m_direct));
	}
	// The Vulkan buffer itself; only immediate-mode schedulers have one on this thread.
	[[nodiscard]] vk::CommandBuffer Native() const {
		EXIT_IF(m_recorder != nullptr);
		return m_direct;
	}
	// Runs `function(vk::CommandBuffer)` where the commands are recorded (escape hatch for raw
	// Vulkan calls; everything it captures must stay valid until the buffer is submitted).
	template <typename F>
	void Raw(F&& function) const {
		if (m_recorder == nullptr) {
			function(m_direct);
		} else {
			m_recorder->Record(std::forward<F>(function));
		}
	}

#define KYTY_CMD_FORWARD(name)                                                                     \
	template <typename... Args>                                                                    \
	void name(Args... args) const {                                                                \
		if (m_recorder == nullptr) {                                                               \
			m_direct.name(args...);                                                                \
		} else {                                                                                   \
			m_recorder->Record([=](vk::CommandBuffer c) { c.name(args...); });                     \
		}                                                                                          \
	}
	// Commands without pointer arguments.
	KYTY_CMD_FORWARD(bindPipeline)
	KYTY_CMD_FORWARD(bindIndexBuffer)
	KYTY_CMD_FORWARD(dispatch)
	KYTY_CMD_FORWARD(dispatchBase)
	KYTY_CMD_FORWARD(dispatchIndirect)
	KYTY_CMD_FORWARD(draw)
	KYTY_CMD_FORWARD(drawIndexed)
	KYTY_CMD_FORWARD(drawIndirect)
	KYTY_CMD_FORWARD(drawIndexedIndirect)
	KYTY_CMD_FORWARD(drawMeshTasksEXT)
	KYTY_CMD_FORWARD(drawMeshTasksIndirectEXT)
	KYTY_CMD_FORWARD(endRendering)
	KYTY_CMD_FORWARD(fillBuffer)
	KYTY_CMD_FORWARD(writeTimestamp)
	KYTY_CMD_FORWARD(resetQueryPool)
	KYTY_CMD_FORWARD(beginQuery)
	KYTY_CMD_FORWARD(endQuery)
	KYTY_CMD_FORWARD(writeBufferMarkerAMD)
	KYTY_CMD_FORWARD(setStencilWriteMask)
	KYTY_CMD_FORWARD(setStencilReference)
	KYTY_CMD_FORWARD(setStencilOp)
	KYTY_CMD_FORWARD(setStencilCompareMask)
	KYTY_CMD_FORWARD(setStencilTestEnable)
	KYTY_CMD_FORWARD(setLineWidth)
	KYTY_CMD_FORWARD(setFrontFace)
	KYTY_CMD_FORWARD(setDepthWriteEnable)
	KYTY_CMD_FORWARD(setDepthTestEnable)
	KYTY_CMD_FORWARD(setDepthCompareOp)
	KYTY_CMD_FORWARD(setDepthBoundsTestEnable)
	KYTY_CMD_FORWARD(setDepthBounds)
	KYTY_CMD_FORWARD(setDepthBiasEnable)
	KYTY_CMD_FORWARD(setDepthBias)
	KYTY_CMD_FORWARD(setCullMode)
	KYTY_CMD_FORWARD(setPrimitiveTopology)
	KYTY_CMD_FORWARD(setPrimitiveRestartEnable)
	KYTY_CMD_FORWARD(setRasterizerDiscardEnable)
	KYTY_CMD_FORWARD(setAttachmentFeedbackLoopEnableEXT)
	KYTY_CMD_FORWARD(endConditionalRenderingEXT)
#undef KYTY_CMD_FORWARD

	void beginConditionalRenderingEXT(const vk::ConditionalRenderingBeginInfoEXT& info) const {
		if (m_recorder == nullptr) {
			m_direct.beginConditionalRenderingEXT(info);
			return;
		}
		EXIT_IF(info.pNext != nullptr);
		m_recorder->Record([info](vk::CommandBuffer c) { c.beginConditionalRenderingEXT(info); });
	}
	void beginConditionalRenderingEXT(const vk::ConditionalRenderingBeginInfoEXT* info) const {
		beginConditionalRenderingEXT(*info);
	}

	// vulkan-hpp array-proxy forms.
	void copyBuffer(vk::Buffer src, vk::Buffer dst, vk::ArrayProxy<const vk::BufferCopy> const& r) const {
		copyBuffer(src, dst, r.size(), r.data());
	}
	void copyImage(vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
	               vk::ArrayProxy<const vk::ImageCopy> const& r) const {
		copyImage(src, sl, dst, dl, r.size(), r.data());
	}
	void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout l,
	                       vk::ArrayProxy<const vk::BufferImageCopy> const& r) const {
		copyBufferToImage(src, dst, l, r.size(), r.data());
	}
	void copyImageToBuffer(vk::Image src, vk::ImageLayout l, vk::Buffer dst,
	                       vk::ArrayProxy<const vk::BufferImageCopy> const& r) const {
		copyImageToBuffer(src, l, dst, r.size(), r.data());
	}
	void resolveImage(vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
	                  vk::ArrayProxy<const vk::ImageResolve> const& r) const {
		resolveImage(src, sl, dst, dl, r.size(), r.data());
	}
	void blitImage(vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
	               vk::ArrayProxy<const vk::ImageBlit> const& r, vk::Filter filter) const {
		blitImage(src, sl, dst, dl, r.size(), r.data(), filter);
	}
	void pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
	                     vk::DependencyFlags flags,
	                     vk::ArrayProxy<const vk::MemoryBarrier> const&       memory,
	                     vk::ArrayProxy<const vk::BufferMemoryBarrier> const& buffers,
	                     vk::ArrayProxy<const vk::ImageMemoryBarrier> const&  images) const {
		pipelineBarrier(src, dst, flags, memory.size(), memory.data(), buffers.size(), buffers.data(),
		                images.size(), images.data());
	}
	void pushDescriptorSetKHR(vk::PipelineBindPoint bp, vk::PipelineLayout layout, uint32_t set,
	                          vk::ArrayProxy<const vk::WriteDescriptorSet> const& writes) const {
		pushDescriptorSetKHR(bp, layout, set, writes.size(), writes.data());
	}
	void bindDescriptorSets(vk::PipelineBindPoint bp, vk::PipelineLayout layout, uint32_t first,
	                        vk::ArrayProxy<const vk::DescriptorSet> const& sets,
	                        vk::ArrayProxy<const uint32_t> const&          dynamic_offsets) const {
		bindDescriptorSets(bp, layout, first, sets.size(), sets.data(), dynamic_offsets.size(),
		                   dynamic_offsets.data());
	}
	template <typename T>
	void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset,
	                   vk::ArrayProxy<const T> const& values) const {
		pushConstants(layout, stages, offset, static_cast<uint32_t>(values.size() * sizeof(T)),
		              values.data());
	}
	void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
	                     vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
		clearColorImage(image, layout, &color, ranges.size(), ranges.data());
	}
	void clearDepthStencilImage(vk::Image image, vk::ImageLayout layout,
	                            const vk::ClearDepthStencilValue&                     value,
	                            vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
		clearDepthStencilImage(image, layout, &value, ranges.size(), ranges.data());
	}
	void clearAttachments(vk::ArrayProxy<const vk::ClearAttachment> const& attachments,
	                      vk::ArrayProxy<const vk::ClearRect> const&       rects) const {
		clearAttachments(attachments.size(), attachments.data(), rects.size(), rects.data());
	}
	void setViewport(uint32_t first, vk::ArrayProxy<const vk::Viewport> const& v) const {
		setViewport(first, v.size(), v.data());
	}
	void setScissor(uint32_t first, vk::ArrayProxy<const vk::Rect2D> const& r) const {
		setScissor(first, r.size(), r.data());
	}
	void setColorWriteEnableEXT(vk::ArrayProxy<const vk::Bool32> const& e) const {
		setColorWriteEnableEXT(e.size(), e.data());
	}

	void pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
	                     vk::DependencyFlags flags, uint32_t memory_count,
	                     const vk::MemoryBarrier* memory, uint32_t buffer_count,
	                     const vk::BufferMemoryBarrier* buffers, uint32_t image_count,
	                     const vk::ImageMemoryBarrier* images) const;
	void pipelineBarrier2(const vk::DependencyInfo& dependency) const;
	void pipelineBarrier2(const vk::DependencyInfo* dependency) const { pipelineBarrier2(*dependency); }
	void copyBuffer(vk::Buffer src, vk::Buffer dst, uint32_t count, const vk::BufferCopy* regions) const;
	void copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst, vk::ImageLayout dst_layout,
	               uint32_t count, const vk::ImageCopy* regions) const;
	void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout, uint32_t count,
	                       const vk::BufferImageCopy* regions) const;
	void copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst, uint32_t count,
	                       const vk::BufferImageCopy* regions) const;
	void resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
	                  vk::ImageLayout dst_layout, uint32_t count, const vk::ImageResolve* regions) const;
	void blitImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst, vk::ImageLayout dst_layout,
	               uint32_t count, const vk::ImageBlit* regions, vk::Filter filter) const;
	void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue* color,
	                     uint32_t count, const vk::ImageSubresourceRange* ranges) const;
	void clearDepthStencilImage(vk::Image image, vk::ImageLayout layout,
	                            const vk::ClearDepthStencilValue* value, uint32_t count,
	                            const vk::ImageSubresourceRange* ranges) const;
	void clearAttachments(uint32_t count, const vk::ClearAttachment* attachments, uint32_t rect_count,
	                      const vk::ClearRect* rects) const;
	void pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, uint32_t set,
	                          uint32_t count, const vk::WriteDescriptorSet* writes) const;
	void bindDescriptorSets(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, uint32_t first,
	                        uint32_t count, const vk::DescriptorSet* sets, uint32_t dynamic_count,
	                        const uint32_t* dynamic_offsets) const;
	void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset,
	                   uint32_t size, const void* values) const;
	void beginRendering(const vk::RenderingInfo& info) const;
	void beginRendering(const vk::RenderingInfo* info) const { beginRendering(*info); }
	void bindVertexBuffers2(uint32_t first, uint32_t count, const vk::Buffer* buffers,
	                        const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
	                        const vk::DeviceSize* strides) const;
	void setViewport(uint32_t first, uint32_t count, const vk::Viewport* viewports) const;
	void setScissor(uint32_t first, uint32_t count, const vk::Rect2D* rects) const;
	void setViewportWithCount(uint32_t count, const vk::Viewport* viewports) const;
	void setScissorWithCount(uint32_t count, const vk::Rect2D* rects) const;
	void setColorWriteEnableEXT(uint32_t count, const vk::Bool32* enables) const;
	void setBlendConstants(const float constants[4]) const;

private:
	CommandRecorder*  m_recorder = nullptr;
	vk::CommandBuffer m_direct;
	uint64_t          m_epoch = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDRECORDER_H_
