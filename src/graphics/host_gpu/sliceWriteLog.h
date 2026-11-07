#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_SLICEWRITELOG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_SLICEWRITELOG_H_

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

// Guest memory the GPU thread changed in the current submission slice (2026-11-07 part 13), for
// look-ahead materialization (KYTY_PREWALK=1, renderer/pipeline/prewalk.h) and its feasibility probe
// (KYTY_PREWALK_PROBE=1). A helper that materializes the slice's draws when the slice starts sees
// guest memory as it is then. During the slice only the GPU thread changes memory the guest may
// rely on: ranges it registers as GPU-written, readbacks it downloads into guest memory and PM4
// packets that write memory. This log records those ranges (page-aligned, merged; GPU thread
// only, other threads' notes are ignored) so a result can be checked against them before use.
namespace Libs::Graphics::SliceWriteLog {

inline bool Enabled() {
	static const bool enabled = [] {
		const char* probe    = std::getenv("KYTY_PREWALK_PROBE");
		const char* prewalk  = std::getenv("KYTY_PREWALK");
		return (probe != nullptr && probe[0] == '1') || prewalk == nullptr || prewalk[0] != '0';
	}();
	return enabled;
}

// The feasibility probe's per-materialization classification (Prewalk* counters).
inline bool ProbeEnabled() {
	static const bool enabled = [] {
		const char* probe = std::getenv("KYTY_PREWALK_PROBE");
		return probe != nullptr && probe[0] == '1';
	}();
	return enabled;
}

struct State {
	bool                                       active = false;
	std::vector<std::pair<uint64_t, uint64_t>> ranges; // [begin, end), sorted, disjoint
};

inline thread_local State t_state;

// GPU thread, at the start of every graphics/compute submission slice.
inline void Begin() {
	if (!Enabled()) return;
	auto& state  = t_state;
	state.active = true;
	state.ranges.clear();
}

inline void Note(uint64_t address, uint64_t size) {
	auto& state = t_state;
	if (!state.active || size == 0) return;
	uint64_t begin  = address & ~uint64_t {0xfff};
	uint64_t end    = (address + size + 0xfffu) & ~uint64_t {0xfff};
	auto&    ranges = state.ranges;
	// First range ending at or after `begin`; merge every range touching [begin, end).
	auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
	                              [](const auto& range, uint64_t value) { return range.second < value; });
	auto last = first;
	while (last != ranges.end() && last->first <= end) {
		begin = std::min(begin, last->first);
		end   = std::max(end, last->second);
		++last;
	}
	first = ranges.erase(first, last);
	ranges.insert(first, {begin, end});
}

[[nodiscard]] inline bool RangeWritten(uint64_t address, uint64_t size) {
	const auto& state = t_state;
	if (!state.active || state.ranges.empty() || size == 0) return false;
	const auto& ranges = state.ranges;
	const auto  found  = std::lower_bound(ranges.begin(), ranges.end(), address,
	                                      [](const auto& range, uint64_t value) { return range.second <= value; });
	return found != ranges.end() && found->first < address + size;
}

[[nodiscard]] inline bool PageWritten(uint64_t page) {
	return RangeWritten(page << 12u, 4096u);
}

} // namespace Libs::Graphics::SliceWriteLog

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_SLICEWRITELOG_H_
