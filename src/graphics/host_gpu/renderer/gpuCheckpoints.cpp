#include "graphics/host_gpu/renderer/gpuCheckpoints.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/gpuProfiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace Libs::Graphics {

namespace {

struct Record {
	std::atomic<uint64_t> sequence {0};
	GpuCheckpointOp       op          = GpuCheckpointOp::Dispatch;
	uint64_t              shader_hash = 0;
	uint32_t              args[3] {};
	uint64_t              address  = 0; // dispatch: first written buffer base
	uint64_t              address2 = 0; // dispatch: first read-only buffer base
	bool                  async    = false; // dispatch from a guest async-compute queue
};

// The GPU writes the low 32 bits of a command's sequence number before (slot 0) and after
// (slot 1) it runs, into host memory that stays readable after a device loss.
constexpr uint64_t               RECORD_COUNT = 1u << 16u;
std::array<Record, RECORD_COUNT> g_records;
std::atomic<uint64_t>            g_next_sequence {1};
std::atomic<bool>                g_enabled {false};
vk::Buffer                       g_marker_buffer = nullptr;
vk::DeviceMemory                 g_marker_memory = nullptr;
volatile uint32_t*               g_markers       = nullptr;
bool                             g_device_fault  = false;
std::atomic<uint64_t>            g_direct_dispatch_seen {0};

// KYTY_GPU_LOG_INDIRECT=1: the GPU copies each indirect draw's first argument record into this
// ring (after the two marker words) right before the draw, so a device-loss report shows the
// arguments the GPU actually read.
constexpr uint32_t ARGS_SLOTS      = 64;
constexpr uint32_t ARGS_SLOT_BYTES = 32;
constexpr uint32_t ARGS_OFFSET     = 64;
struct ArgsSlot {
	uint64_t                 guest_address = 0;
	uint64_t                 sequence      = 0;
	uint32_t                 bytes         = 0;
	uint64_t                 source_offset = 0;
	GpuIndirectArgsHostState host;
};
std::array<ArgsSlot, ARGS_SLOTS> g_args_slots;
std::atomic<uint64_t>            g_args_next {0};

struct DirectDispatchSample {
	uint64_t every = 1;
	uint64_t phase = 0;
};

constexpr bool SelectDirectDispatch(uint64_t ordinal, uint64_t every, uint64_t phase) {
	return ordinal % every == phase;
}

static_assert(SelectDirectDispatch(0, 1, 0));
static_assert(SelectDirectDispatch(4, 4, 0));
static_assert(!SelectDirectDispatch(1, 4, 0));
static_assert(SelectDirectDispatch(6, 4, 2));
static_assert(!SelectDirectDispatch(0, 4, 2));

uint64_t ReadPositiveEnv(const char* name, uint64_t fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') {
		return fallback;
	}
	char* end = nullptr;
	errno     = 0;
	const auto parsed = std::strtoull(value, &end, 0);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
		std::printf("GPU checkpoints: ignoring invalid %s=%s\n", name, value);
		return fallback;
	}
	return parsed;
}

const DirectDispatchSample& DirectDispatchSampling() {
	static const DirectDispatchSample sample = [] {
		DirectDispatchSample result;
		result.every = ReadPositiveEnv("KYTY_GPU_CHECKPOINTS_DISPATCH_EVERY", 1);
		const char* phase_value = std::getenv("KYTY_GPU_CHECKPOINTS_DISPATCH_PHASE");
		if (phase_value != nullptr && phase_value[0] != '\0') {
			char* end = nullptr;
			errno     = 0;
			const auto phase = std::strtoull(phase_value, &end, 0);
			if (errno != 0 || end == phase_value || *end != '\0' || phase >= result.every) {
				std::printf("GPU checkpoints: ignoring invalid "
				            "KYTY_GPU_CHECKPOINTS_DISPATCH_PHASE=%s for period %" PRIu64 "\n",
				            phase_value, result.every);
			} else {
				result.phase = phase;
			}
		}
		return result;
	}();
	return sample;
}

uint64_t SelectedDirectDispatches(uint64_t seen, const DirectDispatchSample& sample) {
	return seen <= sample.phase ? 0 : 1 + (seen - 1 - sample.phase) / sample.every;
}

void ReportDeviceFault(const GraphicContext& graphics) {
	if (!g_device_fault) {
		return;
	}
	vk::DeviceFaultCountsEXT counts {};
	if (graphics.device.getFaultInfoEXT(&counts, nullptr) != vk::Result::eSuccess &&
	    counts.addressInfoCount == 0 && counts.vendorInfoCount == 0) {
		std::printf("Device fault: no information\n");
		return;
	}
	std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
	std::vector<vk::DeviceFaultVendorInfoEXT>  vendor(counts.vendorInfoCount);
	counts.vendorBinarySize = 0;
	vk::DeviceFaultInfoEXT info {};
	info.pAddressInfos = addresses.data();
	info.pVendorInfos  = vendor.data();
	const auto result  = graphics.device.getFaultInfoEXT(&counts, &info);
	std::printf("Device fault (%s): %s\n", vk::to_string(result).c_str(), info.description.data());
	for (const auto& address: addresses) {
		std::printf("  address type=%s reported=0x%016" PRIx64 " precision=0x%" PRIx64 "\n",
		            vk::to_string(address.addressType).c_str(), address.reportedAddress,
		            address.addressPrecision);
	}
	for (const auto& entry: vendor) {
		std::printf("  vendor: %s code=0x%" PRIx64 " data=0x%" PRIx64 "\n", entry.description.data(),
		            entry.vendorFaultCode, entry.vendorFaultData);
	}
}

const char* OpName(GpuCheckpointOp op) {
	switch (op) {
		case GpuCheckpointOp::Dispatch: return "dispatch";
		case GpuCheckpointOp::DispatchIndirect: return "dispatch-indirect";
		case GpuCheckpointOp::Draw: return "draw";
		case GpuCheckpointOp::Copy: return "copy";
	}
	return "?";
}

void PrintRecord(const char* label, uint64_t sequence) {
	const auto& record = g_records[sequence % RECORD_COUNT];
	const bool  known  = record.sequence.load(std::memory_order_acquire) == sequence;
	if (record.op == GpuCheckpointOp::Copy && known) {
		std::printf("  %s #%" PRIu64 ": emulator work at %s:%u", label, sequence,
		            reinterpret_cast<const char*>(record.shader_hash), record.args[0]);
		if (record.args[1] != 0 || record.args[2] != 0 || record.address != 0 ||
		    record.address2 != 0) {
			std::printf(" detail=%u,%u,0x%" PRIx64 ",0x%" PRIx64, record.args[1], record.args[2],
			            record.address, record.address2);
		}
		std::printf("\n");
		return;
	}
	std::printf("  %s #%" PRIu64 ": %s shader=0x%016" PRIx64 " args=%u,%u,%u address=0x%016" PRIx64
	            " read=0x%016" PRIx64 "%s\n",
	            label, sequence, OpName(record.op), record.shader_hash, record.args[0],
	            record.args[1], record.args[2], record.address, record.address2,
	            known ? "" : " (record reused)");
}

} // namespace

static bool AllCheckpointsRequested() {
	const char* value = std::getenv("KYTY_GPU_CHECKPOINTS");
	return value != nullptr && value[0] == '1';
}

static uint32_t CheckpointMask() {
	static const uint32_t mask = [] {
		if (!AllCheckpointsRequested()) {
			return (1u << static_cast<uint32_t>(GpuCheckpointOp::DispatchIndirect)) |
			       (1u << static_cast<uint32_t>(GpuCheckpointOp::Copy));
		}
		const char* value = std::getenv("KYTY_GPU_CHECKPOINTS_MASK");
		return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : ~0u;
	}();
	return mask;
}

bool GpuCheckpointsRequested() {
	const char* value = std::getenv("KYTY_INDIRECT_MARKERS");
	return AllCheckpointsRequested() || value == nullptr || value[0] != '0';
}

void InitGpuCheckpoints(GraphicContext& graphics) {
	const vk::BufferCreateInfo buffer_info {.size  = ARGS_OFFSET + ARGS_SLOTS * ARGS_SLOT_BYTES,
	                                        .usage = vk::BufferUsageFlagBits::eTransferDst};
	if (graphics.device.createBuffer(&buffer_info, nullptr, &g_marker_buffer) != vk::Result::eSuccess) {
		return;
	}
	vk::MemoryRequirements requirements {};
	graphics.device.getBufferMemoryRequirements(g_marker_buffer, &requirements);
	const auto& memory = graphics.physical_device_memory_properties;
	constexpr auto wanted = vk::MemoryPropertyFlagBits::eHostVisible |
	                        vk::MemoryPropertyFlagBits::eHostCoherent;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++) {
		const auto flags = memory.memoryTypes[i].propertyFlags;
		// System memory survives a device reset, so avoid device-local host-visible heaps.
		if ((requirements.memoryTypeBits & (1u << i)) != 0 && (flags & wanted) == wanted &&
		    !(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
			type = i;
		}
	}
	if (type == UINT32_MAX) {
		return;
	}
	const vk::MemoryAllocateInfo allocate {.allocationSize = requirements.size, .memoryTypeIndex = type};
	void*                         mapped = nullptr;
	if (graphics.device.allocateMemory(&allocate, nullptr, &g_marker_memory) != vk::Result::eSuccess ||
	    graphics.device.bindBufferMemory(g_marker_buffer, g_marker_memory, 0) != vk::Result::eSuccess ||
	    graphics.device.mapMemory(g_marker_memory, 0, VK_WHOLE_SIZE, {}, &mapped) != vk::Result::eSuccess) {
		return;
	}
	g_markers    = static_cast<volatile uint32_t*>(mapped);
	g_markers[0] = 0;
	g_markers[1] = 0;
	g_enabled.store(true, std::memory_order_release);
	if ((CheckpointMask() & (1u << static_cast<uint32_t>(GpuCheckpointOp::Dispatch))) != 0) {
		const auto& sample = DirectDispatchSampling();
		std::printf("GPU checkpoint direct dispatch sample: ordinal %% %" PRIu64 " == %" PRIu64
		            "\n",
		            sample.every, sample.phase);
	}
}

void SetDeviceFaultReporting(bool enabled) {
	g_device_fault = enabled;
}

bool GpuCheckpointsEnabled() {
	return g_enabled.load(std::memory_order_relaxed);
}

uint64_t GpuCheckpointBegin(vk::CommandBuffer command, GpuCheckpointOp op, uint64_t shader_hash,
                            uint32_t arg0, uint32_t arg1, uint32_t arg2, uint64_t address,
                            uint64_t address2, bool async_compute) {
	GpuProfileBegin(command, op, shader_hash, arg0, arg1);
	if (!GpuCheckpointsEnabled()) {
		return 0;
	}
	// KYTY_GPU_CHECKPOINTS_MASK selects the command kinds that get markers (bit per op).
	const uint32_t mask = CheckpointMask();
	if ((mask & (1u << static_cast<uint32_t>(op))) == 0) {
		return 0;
	}
	if (op == GpuCheckpointOp::Dispatch) {
		const auto ordinal = g_direct_dispatch_seen.fetch_add(1, std::memory_order_relaxed);
		const auto& sample = DirectDispatchSampling();
		if (!SelectDirectDispatch(ordinal, sample.every, sample.phase)) {
			return 0;
		}
	}
	const auto sequence = g_next_sequence.fetch_add(1, std::memory_order_relaxed);
	auto&      record   = g_records[sequence % RECORD_COUNT];
	record.op           = op;
	record.shader_hash  = shader_hash;
	record.args[0]      = arg0;
	record.args[1]      = arg1;
	record.args[2]      = arg2;
	record.address      = address;
	record.address2     = address2;
	record.async        = async_compute;
	record.sequence.store(sequence, std::memory_order_release);
	command.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eTopOfPipe, g_marker_buffer, 0,
	                             static_cast<uint32_t>(sequence));
	return sequence;
}

void GpuCheckpointEnd(vk::CommandBuffer command, uint64_t sequence) {
	GpuProfileEnd(command);
	if (sequence == 0 || !GpuCheckpointsEnabled()) {
		return;
	}
	// KYTY_GPU_CHECKPOINTS_TOP_ONLY=1: direct dispatches get only the top-of-pipe "started"
	// marker. A bottom-of-pipe marker waits for all earlier work, which serializes dispatches
	// and hides NHL 27's device loss.
	static const bool top_only = [] {
		const char* value = std::getenv("KYTY_GPU_CHECKPOINTS_TOP_ONLY");
		return value != nullptr && value[0] == '1';
	}();
	// KYTY_GPU_CHECKPOINTS_BOTTOM_EVERY=N keeps the completion marker on every Nth of those
	// commands, so the last completed marker is within N commands of the stall.
	static const uint64_t bottom_every = ReadPositiveEnv("KYTY_GPU_CHECKPOINTS_BOTTOM_EVERY", 0);
	const auto            op           = g_records[sequence % RECORD_COUNT].op;
	if (top_only && (op == GpuCheckpointOp::Dispatch || op == GpuCheckpointOp::Draw) &&
	    (bottom_every == 0 || sequence % bottom_every != 0)) {
		return;
	}
	command.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, g_marker_buffer, 4,
	                             static_cast<uint32_t>(sequence));
}

namespace {
thread_local uint32_t           t_upload_reason = 0;
thread_local GpuCurrentDispatch t_current_dispatch;
} // namespace

void GpuNoteCurrentDispatch(const GpuCurrentDispatch& dispatch) {
	t_current_dispatch = dispatch;
}

const GpuCurrentDispatch& GpuGetCurrentDispatch() {
	return t_current_dispatch;
}

void ReportRetiredRecording(const char* what, uint64_t shader_hash) {
	static std::atomic<uint32_t> reported {0};
	if (reported.fetch_add(1, std::memory_order_relaxed) < 50) {
		std::printf("RETIRED RECORDING: %s 0x%016" PRIx64
		            " was recorded into an already submitted command buffer\n",
		            what, shader_hash);
		std::fflush(stdout);
	}
}

void GpuCheckpointSetUploadReason(uint32_t reason) {
	t_upload_reason = reason;
}

uint32_t GpuCheckpointTakeUploadReason() {
	const auto reason = t_upload_reason;
	t_upload_reason   = 0;
	return reason;
}

bool GpuIndirectArgsLogEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_LOG_INDIRECT");
		return value != nullptr && value[0] == '1';
	}();
	return enabled && GpuCheckpointsEnabled();
}

void GpuLogIndirectArgs(vk::CommandBuffer command, vk::Buffer source, uint64_t offset,
                        uint64_t guest_address, uint32_t bytes,
                        const GpuIndirectArgsHostState& host) {
	if (!GpuIndirectArgsLogEnabled()) {
		return;
	}
	const auto index = g_args_next.fetch_add(1, std::memory_order_relaxed);
	const auto slot  = static_cast<uint32_t>(index % ARGS_SLOTS);
	bytes            = std::min(bytes, ARGS_SLOT_BYTES) & ~3u;
	auto& meta       = g_args_slots[slot];
	if (index >= ARGS_SLOTS) {
		// 64 logs later the previous copy into this slot must have executed.
		const volatile uint32_t* previous = g_markers + (ARGS_OFFSET + slot * ARGS_SLOT_BYTES) / 4;
		const uint32_t own = 0xd0000000u | static_cast<uint32_t>((index - ARGS_SLOTS) & 0x0fffffffu);
		if ((previous[0] & 0xf0000000u) == 0xd0000000u && meta.bytes != 0) {
			static std::atomic<uint32_t> logged {0};
			if (logged.fetch_add(1, std::memory_order_relaxed) < 100) {
				std::printf("indirect args copy %s: id=0x%08x guest=0x%016" PRIx64
				            " before record #%" PRIu64 " sentinel=0x%08x own=0x%08x words=%08x %08x %08x"
				            " fill=%08x src_offset=0x%" PRIx64 " src_size=0x%" PRIx64
				            " markers=%08x/%08x cb=0x%" PRIx64 " tick=%" PRIu64 "\n",
				            previous[0] == own ? "never executed" : "copied a sentinel",
				            meta.host.count, meta.guest_address, meta.sequence, previous[0], own,
				            previous[0], previous[1], previous[2], previous[ARGS_SLOT_BYTES / 4 - 1],
				            meta.source_offset, meta.host.source_size, g_markers[0], g_markers[1],
				            meta.host.command, meta.host.tick);
			}
		}
	}
	meta.guest_address = guest_address;
	meta.sequence      = g_next_sequence.load(std::memory_order_relaxed);
	meta.bytes         = bytes;
	meta.host          = host;
	meta.source_offset = offset;
	meta.host.command  = reinterpret_cast<uint64_t>(static_cast<VkCommandBuffer>(command));
	if (index < 40) {
		std::printf("indirect args log #%" PRIu64 " id=0x%08x cb=0x%" PRIx64 " tick=%" PRIu64
		            " before record #%" PRIu64 "\n",
		            index, host.count, meta.host.command, host.tick, meta.sequence);
	}
	volatile uint32_t* words = g_markers + (ARGS_OFFSET + slot * ARGS_SLOT_BYTES) / 4;
	for (uint32_t i = 0; i < ARGS_SLOT_BYTES / 4; i++) {
		words[i] = 0xd0000000u | static_cast<uint32_t>(index & 0x0fffffffu);
	}
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = source;
	before.offset              = offset;
	before.size                = bytes;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                        nullptr);
	const vk::BufferCopy copy {offset, ARGS_OFFSET + slot * ARGS_SLOT_BYTES, bytes};
	command.copyBuffer(source, g_marker_buffer, 1, &copy);
	// A fill right after the copy tells a dropped copy from a skipped command range.
	command.fillBuffer(g_marker_buffer, ARGS_OFFSET + slot * ARGS_SLOT_BYTES + ARGS_SLOT_BYTES - 4,
	                   4, 0xfeed0000u | static_cast<uint32_t>(index & 0xffffu));
}

void ReportGpuCheckpoints(const GraphicContext& graphics) {
	static std::atomic<bool> reported {false};
	if (reported.exchange(true)) {
		return;
	}
	ReportDeviceFault(graphics);
	if (!GpuCheckpointsEnabled()) {
		std::fflush(stdout);
		return;
	}
	const auto last    = g_next_sequence.load(std::memory_order_relaxed) - 1u;
	// Expand the 32-bit markers relative to the newest recorded sequence.
	const auto expand  = [last](uint32_t marker) {
        return (last & ~uint64_t {0xffffffffu}) | marker;
	};
	const auto started   = expand(g_markers[0]);
	const auto completed = expand(g_markers[1]);
	std::printf("GPU buffer markers after device loss: last started #%" PRIu64
	            ", last completed #%" PRIu64 ", last recorded #%" PRIu64 "\n",
	            started, completed, last);
	if ((CheckpointMask() & (1u << static_cast<uint32_t>(GpuCheckpointOp::Dispatch))) != 0) {
		const auto& sample = DirectDispatchSampling();
		const auto seen    = g_direct_dispatch_seen.load(std::memory_order_relaxed);
		std::printf("  direct dispatch sample: ordinal %% %" PRIu64 " == %" PRIu64
		            ", selected=%" PRIu64 "/%" PRIu64 "\n",
		            sample.every, sample.phase, SelectedDirectDispatches(seen, sample), seen);
	}
	for (uint64_t sequence = completed > 3 ? completed - 3 : 1; sequence <= completed; sequence++) {
		PrintRecord("completed", sequence);
	}
	// Everything after the last completed marker up to the last started one may be running;
	// list the newest 48 of them.
	// The oldest ones are the likeliest culprits (top-of-pipe markers run ahead of completion).
	const auto in_flight_first = started > completed + 72 ? started - 47 : completed + 1;
	if (in_flight_first > completed + 1) {
		for (uint64_t sequence = completed + 1; sequence <= completed + 24; sequence++) {
			PrintRecord("IN FLIGHT", sequence);
		}
		std::printf("  ... %" PRIu64 " started records omitted\n",
		            in_flight_first - completed - 25);
	}
	for (uint64_t sequence = in_flight_first; sequence <= started; sequence++) {
		PrintRecord("IN FLIGHT", sequence);
	}
	if (GpuIndirectArgsLogEnabled()) {
		const auto next = g_args_next.load(std::memory_order_relaxed);
		std::printf("Indirect draw arguments as read by the GPU (newest last; deadbeef = copy "
		            "not executed):\n");
		for (uint64_t i = next > 16 ? next - 16 : 0; i < next; i++) {
			const auto  slot = static_cast<uint32_t>(i % ARGS_SLOTS);
			const auto& meta = g_args_slots[slot];
			const volatile uint32_t* words =
			    g_markers + (ARGS_OFFSET + slot * ARGS_SLOT_BYTES) / 4;
			std::printf("  #%" PRIu64 " before record #%" PRIu64 " guest=0x%016" PRIx64 ":", i,
			            meta.sequence, meta.guest_address);
			for (uint32_t w = 0; w < meta.bytes / 4; w++) {
				std::printf(" %08x", words[w]);
			}
			std::printf(" fill=%08x src_offset=0x%" PRIx64 " src_size=0x%" PRIx64
			            " cb=0x%" PRIx64 " tick=%" PRIu64,
			            words[ARGS_SLOT_BYTES / 4 - 1], meta.source_offset, meta.host.source_size,
			            meta.host.command, meta.host.tick);
			std::printf("\n      host: count=%u stride=%u gpu_dirty=%d cpu_dirty=%d backing%s",
			            meta.host.count, meta.host.stride, meta.host.gpu_dirty ? 1 : 0,
			            meta.host.cpu_dirty ? 1 : 0, meta.host.backing_valid ? "" : "(invalid)");
			for (uint32_t w = 0; w < 5; w++) {
				std::printf(" %08x", meta.host.backing[w]);
			}
			std::printf("\n");
			PrintRecord(meta.sequence <= completed ? "    next record (completed)"
			                                       : "    next record (not completed)",
			            meta.sequence);
		}
	}
	if (GpuIndirectArgsLogEnabled() && g_args_next.load(std::memory_order_relaxed) != 0) {
		// Candidate producers of the newest indirect record: dispatches whose first written
		// buffer starts at most 4 MiB below it, newest first.
		const auto  newest = g_args_next.load(std::memory_order_relaxed) - 1;
		const auto& meta   = g_args_slots[newest % ARGS_SLOTS];
		const auto  target = meta.guest_address;
		std::printf("Dispatches writing up to 4 MiB below 0x%016" PRIx64
		            " (draw recorded before #%" PRIu64 "), newest first:\n",
		            target, meta.sequence);
		uint32_t found = 0;
		for (uint64_t back = 0; back < RECORD_COUNT - 1 && back < last && found < 16; back++) {
			const auto  sequence = last - back;
			const auto& record   = g_records[sequence % RECORD_COUNT];
			if (record.sequence.load(std::memory_order_acquire) != sequence) {
				continue;
			}
			const auto written = record.op == GpuCheckpointOp::Dispatch         ? record.address
			                     : record.op == GpuCheckpointOp::DispatchIndirect ? record.address2
			                                                                      : 0;
			if (written == 0 || written > target || target - written >= (4u << 20u)) {
				continue;
			}
			found++;
			std::printf("  #%" PRIu64 " %s%s shader=0x%016" PRIx64 " groups=%u,%u,%u written=0x%016" PRIx64
			            " (+0x%" PRIx64 ") read=0x%016" PRIx64 "\n",
			            sequence, record.op == GpuCheckpointOp::Dispatch ? "dispatch" : "dispatch-indirect",
			            record.async ? " [async compute]" : "", record.shader_hash, record.args[0],
			            record.args[1], record.args[2], written, target - written,
			            record.op == GpuCheckpointOp::Dispatch ? record.address2 : record.address);
		}
	}
	{
		// The image the last started command reads (indirect dispatch: address2; dispatch:
		// address2 = first read resource): who wrote or re-uploaded it recently?
		const auto& stalled = g_records[started % RECORD_COUNT];
		const auto  image   = stalled.op == GpuCheckpointOp::DispatchIndirect ? stalled.address2
		                      : stalled.op == GpuCheckpointOp::Dispatch       ? stalled.address2
		                                                                      : 0;
		if (image != 0) {
			std::printf("Records touching 0x%016" PRIx64 " (read by #%" PRIu64
			            ") in the last 60000, newest first (uploads: detail3 bit56 = GPU "
			            "buffer writes, bit57 = CPU writes):\n",
			            image, started);
			uint32_t found = 0;
			for (uint64_t back = 1; back < 60000 && back < started && found < 24; back++) {
				const auto  sequence = started - back;
				const auto& record   = g_records[sequence % RECORD_COUNT];
				if (record.sequence.load(std::memory_order_acquire) != sequence) {
					continue;
				}
				const bool upload = record.op == GpuCheckpointOp::Copy && record.address == image;
				// Buffer or image writes anywhere inside the image's first 8 MiB.
				const bool writer =
				    (record.op == GpuCheckpointOp::Dispatch && record.address >= image &&
				     record.address < image + (8u << 20u)) ||
				    (record.op == GpuCheckpointOp::Draw && record.address2 >= image &&
				     record.address2 < image + (8u << 20u));
				const bool reader = (record.op == GpuCheckpointOp::Dispatch ||
				                     record.op == GpuCheckpointOp::DispatchIndirect) &&
				                    record.address2 == image;
				if (upload || writer || reader) {
					found++;
					PrintRecord(upload ? "UPLOAD" : writer ? "WRITER" : "reader", sequence);
				}
			}
		}
	}
	const auto pending_begin = std::max(started, completed) + 1;
	for (uint64_t sequence = pending_begin, count = 0;
	     sequence <= last && count < 24; sequence++, count++) {
		PrintRecord("RECORDED, NOT OBSERVED STARTED", sequence);
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics
