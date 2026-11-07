#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <atomic>
#include <array>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	// Queue after the range's state change becomes visible, or a drain could consume it first.
	void                   QueueBdaSync(uint64_t vaddr, uint64_t size);
	// CPU writes to guest memory (write faults, new mappings). With slice sync
	// (BdaSliceSyncEnabled) they reach DMA shaders once per guest submission slice instead of
	// before every DMA draw: the guest can only rely on writes made before it submitted (or before
	// a label the command buffer waits on, which ends a slice).
	void                   QueueBdaSyncCpu(uint64_t vaddr, uint64_t size);
	// all: also take the CPU-write queue (slice start, or every draw without slice sync).
	[[nodiscard]] RangeSet TakeBdaSync(bool all = true);
	[[nodiscard]] static bool BdaSliceSyncEnabled();
	// Guest-memory mirror (KYTY_GPU_MIRROR=1, implied by KYTY_GPU_BUFFER_DESCRIPTORS=1): every
	// GPU-visible guest mapping gets pinned host buffers and BDA page-table entries, so shaders
	// reading guest memory through the page table never meet an unmapped page. Mappings are
	// queued from any thread and mirrored on the GPU thread (ApplyMirror, before BDA syncs).
	[[nodiscard]] static bool MirrorEnabled();
	void                      QueueMirror(uint64_t vaddr, uint64_t size);
	void                      ApplyMirror();
	// GPU thread: an unmapped range leaves the mirror (its buffers become evictable).
	void                      UnmapMirror(uint64_t vaddr, uint64_t size);
	// Host-memory mirror (KYTY_GPU_HOST_MIRROR=1, implied by KYTY_GPU_BUFFER_DESCRIPTORS=1):
	// direct-memory mappings are imported zero-copy (VK_EXT_external_memory_host), and every BDA
	// page-table entry no device buffer owns points into that import. GPU reads of such pages
	// see the CPU's current bytes over PCIe: no uploads, no write tracking, no first-touch zeros.
	// Pages a device buffer owns keep pointing at it (its data, GPU writes included, is newer).
	[[nodiscard]] static bool HostMirrorEnabled();
	[[nodiscard]] bool        HasHostImports(uint64_t vaddr, uint64_t size) const;
	// A recorded fault page that only the host mirror backs (a promotion request). With
	// KYTY_GPU_HOST_MIRROR_PROMOTE=0 such pages are not turned into device buffers (diagnostic).
	[[nodiscard]] bool        SkipHostMirrorPromotion(uint64_t page_vaddr) const;
	void               RunGarbageCollector();
	// Inside a batch, CPU-data uploads share one barrier pair: they are recorded together at
	// the end of the outermost batch, or before any other command is recorded (see
	// CommandBuffer::Handle). KYTY_BATCH_UPLOADS=0 disables batching.
	void BeginUploadBatch() noexcept { m_upload_batch_depth++; }
	void EndUploadBatch();
	void FlushUploads();

	class UploadBatchScope {
	public:
		explicit UploadBatchScope(BufferCache& cache) noexcept: m_cache(cache) {
			m_cache.BeginUploadBatch();
		}
		~UploadBatchScope() { m_cache.EndUploadBatch(); }
		UploadBatchScope(const UploadBatchScope&)            = delete;
		UploadBatchScope& operator=(const UploadBatchScope&) = delete;

	private:
		BufferCache& m_cache;
	};

private:
	friend struct BufferCacheTestAccess;

	bool IsBufferInvalid(BufferId id) const { return !id || m_slot_buffers[id].is_deleted; }

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8>                           m_checked_color_metadata;
	MemoryTracker                                     m_memory_tracker;
	std::mutex                                        m_bda_sync_mutex;
	RangeSet                                          m_bda_sync;
	RangeSet                                          m_bda_sync_cpu;
	struct HostImport {
		vk::DeviceMemory  memory  = nullptr;
		vk::Buffer        buffer  = nullptr;
		vk::DeviceAddress address = 0;
		uint64_t          size    = 0;
	};
	// By guest (= host) address.
	std::map<uint64_t, HostImport>                    m_host_imports;
	bool              ImportHostRange(uint64_t vaddr, uint64_t size);
	void              ReleaseHostImports(uint64_t vaddr, uint64_t size);
	// Page-table entries of [vaddr, vaddr + size) that no buffer owns: the import, else 0.
	void              WriteFallbackEntries(uint64_t vaddr, uint64_t size);
	[[nodiscard]] vk::DeviceAddress FallbackAddress(uint64_t page_vaddr) const;
	std::mutex                                        m_mirror_mutex;
	RangeSet                                          m_mirror_pending;
	std::atomic<bool>                                 m_mirror_has_pending {false};
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	struct PendingUpload {
		vk::Buffer     source;
		vk::Buffer     destination;
		vk::BufferCopy copy;
	};
	std::vector<PendingUpload> m_pending_uploads;
	// Tick of the latest GPU write per guest range (ObtainBuffer with is_written).
	TickRangeMap               m_write_ticks;
	// KYTY_RB_TRACE=1: code address of the latest writer per range (readback attribution).
	TickRangeMap               m_write_writers;
	// Downloads whose producers are already submitted run on GraphicContext::readback_queue,
	// waiting only for the producer's tick instead of everything queued on the main queue.
	[[nodiscard]] bool DownloadOnSideQueue(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                       uint64_t wait_tick);
	vk::CommandPool            m_side_pool    = nullptr;
	vk::CommandBuffer          m_side_command = nullptr;
	vk::Fence                  m_side_fence   = nullptr;
	std::unique_ptr<Buffer>    m_side_download;
	uint32_t                   m_upload_batch_depth = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
