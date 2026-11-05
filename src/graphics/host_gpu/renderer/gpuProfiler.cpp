#include "graphics/host_gpu/renderer/gpuProfiler.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace {

using Clock = std::chrono::steady_clock;

// Each command buffer takes one chunk of queries: query 0 opens the buffer, 1 closes it, then
// two per command.
constexpr uint32_t CHUNK_QUERIES = 16384;
constexpr uint32_t CHUNK_COUNT   = 96;

struct Record {
	GpuCheckpointOp op;
	uint64_t        key;
	uint32_t        arg0;
	uint32_t        arg1;
	uint32_t        query; // begin; end is query + 1
	uint32_t        line;  // dispatch: 1 = guest async compute
	bool            ended;
};

struct Chunk {
	CommandScheduler*   scheduler = nullptr;
	uint32_t            first     = 0;
	uint32_t            next      = 0;
	uint64_t            tick      = 0;
	std::vector<Record> records;
	std::vector<size_t> open; // indices into records
	bool                closed    = false;
	bool                overflow  = false;
};

struct Stats {
	GpuCheckpointOp op {};
	uint64_t        key   = 0;
	uint32_t        line  = 0;
	uint64_t        calls = 0;
	double          ns    = 0;
	uint64_t        arg0  = 0; // summed (index count, group count x)
	uint64_t        arg1  = 0;
};

struct Profiler {
	std::mutex                                      mutex;
	std::once_flag                                  init_once;
	std::atomic_bool                                enabled {false};
	double                                          delay_s   = 0;
	double                                          duration_s = 3600;
	Clock::time_point                               start;
	Clock::time_point                               last_report;
	GraphicContext*                                 graphics = nullptr;
	vk::QueryPool                                   pool     = nullptr;
	double                                          period   = 1.0;
	std::vector<uint32_t>                           free_chunks;
	std::unordered_map<uint64_t, Chunk>             active;
	std::deque<Chunk>                               pending;
	std::unordered_map<uint64_t, Stats>             stats;
	std::vector<uint64_t>                           scratch;
	double                                          window_first_s = -1;
	double                                          window_last_s  = 0;
	double                                          marked_ns   = 0;
	double                                          span_ns     = 0;
	uint64_t                                        buffers     = 0;
	uint64_t                                        overflows   = 0;
	uint64_t                                        no_chunk    = 0;
	bool                                            final_written = false;
};

thread_local bool t_async_compute = false;

Profiler& P() {
	static Profiler profiler;
	return profiler;
}

void ParseEnv(Profiler& p) {
	const char* env = std::getenv("KYTY_GPU_PROFILE");
	if (env == nullptr || *env == '\0' || *env == '0') {
		return;
	}
	double delay = 0, duration = 3600;
	if (std::sscanf(env, "%lf,%lf", &delay, &duration) < 2) {
		delay    = 0;
		duration = 3600;
	}
	p.delay_s    = std::max(0.0, delay);
	p.duration_s = std::max(1.0, duration);
	p.start      = Clock::now();
	p.enabled    = true;
	std::printf("GPU profile: window %.0f s + %.0f s -> gpu-profile.txt\n", p.delay_s,
	            p.duration_s);
}

double Seconds(const Profiler& p) {
	return std::chrono::duration<double>(Clock::now() - p.start).count();
}

bool InWindow(const Profiler& p) {
	const double t = Seconds(p);
	return t >= p.delay_s && t < p.delay_s + p.duration_s;
}

uint64_t StatsKey(GpuCheckpointOp op, uint64_t key, uint32_t line) {
	uint64_t k = key * 0x9E3779B97F4A7C15ull;
	k ^= (static_cast<uint64_t>(op) << 56) ^ line;
	return k;
}

const char* OpName(GpuCheckpointOp op) {
	switch (op) {
		case GpuCheckpointOp::Dispatch: return "dispatch";
		case GpuCheckpointOp::DispatchIndirect: return "dispatch-ind";
		case GpuCheckpointOp::Draw: return "draw";
		case GpuCheckpointOp::Copy: return "emulator";
	}
	return "?";
}

const char* KindName(const Stats& s) {
	if (s.line == 1 && s.op == GpuCheckpointOp::Dispatch) {
		return "dispatch-acb";
	}
	if (s.line == 1 && s.op == GpuCheckpointOp::DispatchIndirect) {
		return "dispatch-ind-acb";
	}
	return OpName(s.op);
}

void WriteReport(Profiler& p) {
	std::vector<const Stats*> sorted;
	sorted.reserve(p.stats.size());
	for (const auto& [k, s]: p.stats) {
		sorted.push_back(&s);
	}
	std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->ns > b->ns; });
	std::FILE* out = std::fopen("gpu-profile.txt", "w");
	if (out == nullptr) {
		return;
	}
	const double window = std::max(p.window_last_s - p.window_first_s, 1e-3);
	std::fprintf(out,
	             "GPU profile: %.1f s of results, %" PRIu64 " command buffers (%" PRIu64
	             " overflowed, %" PRIu64 " unprofiled)\n",
	             window, p.buffers, p.overflows, p.no_chunk);
	std::fprintf(out,
	             "command-buffer span %.1f ms/s, marked commands %.1f ms/s, unmarked %.1f ms/s\n",
	             p.span_ns / window / 1e6, p.marked_ns / window / 1e6,
	             (p.span_ns - p.marked_ns) / window / 1e6);
	std::fprintf(out, "Bottom-of-pipe timestamps serialize the queue; shares are attribution.\n\n");
	std::fprintf(out, "%7s %9s %9s %9s %10s  %-12s %s\n", "share", "ms/s", "calls/s", "avg_us",
	             "avg_arg0", "kind", "shader / source");
	double covered = 0;
	int    rank    = 0;
	for (const auto* s: sorted) {
		if (++rank > 80) {
			break;
		}
		covered += s->ns;
		const double share = p.span_ns > 0 ? 100.0 * s->ns / p.span_ns : 0;
		std::fprintf(out, "%6.2f%% %9.2f %9.1f %9.1f %10.0f  %-12s ", share, s->ns / window / 1e6,
		             static_cast<double>(s->calls) / window, s->ns / static_cast<double>(s->calls) / 1e3,
		             static_cast<double>(s->arg0) / static_cast<double>(s->calls), KindName(*s));
		if (s->op == GpuCheckpointOp::Copy) {
			const char* file  = reinterpret_cast<const char*>(s->key);
			const char* slash = file;
			for (const char* c = file; *c != '\0'; c++) {
				if (*c == '/' || *c == '\\') {
					slash = c + 1;
				}
			}
			std::fprintf(out, "%s:%u\n", slash, s->line);
		} else {
			std::fprintf(out, "0x%016" PRIx64 "\n", s->key);
		}
	}
	std::fprintf(out, "\n%zu distinct entries; top 80 cover %.1f%% of marked time\n",
	             p.stats.size(), p.marked_ns > 0 ? 100.0 * covered / p.marked_ns : 0);
	// KYTY_PROFILE_WATCH=hash[,hash...]: always list these shaders (also when absent).
	if (const char* watch = std::getenv("KYTY_PROFILE_WATCH")) {
		std::fprintf(out, "\nwatched shaders:\n");
		for (const char* q = watch; *q != '\0';) {
			char*          end  = nullptr;
			const uint64_t hash = std::strtoull(q, &end, 16);
			if (end == q) break;
			bool any = false;
			for (const auto* s: sorted) {
				if (s->key == hash && s->op != GpuCheckpointOp::Copy) {
					any = true;
					std::fprintf(out, "  0x%016" PRIx64 " %-12s calls/s=%.1f avg_us=%.1f\n", hash,
					             KindName(*s), static_cast<double>(s->calls) / window,
					             s->ns / static_cast<double>(s->calls) / 1e3);
				}
			}
			if (!any) {
				std::fprintf(out, "  0x%016" PRIx64 " never executed on the GPU in the window\n", hash);
			}
			q = *end == ',' ? end + 1 : end;
		}
	}
	std::fclose(out);
}

// Reads every finished chunk; called with the mutex held.
void Collect(Profiler& p) {
	while (!p.pending.empty()) {
		auto& chunk = p.pending.front();
		if (!chunk.scheduler->IsFree(chunk.tick)) {
			break;
		}
		const uint32_t count = chunk.next - chunk.first;
		p.scratch.resize(count);
		const auto result = p.graphics->device.getQueryPoolResults(
		    p.pool, chunk.first, count, count * sizeof(uint64_t), p.scratch.data(),
		    sizeof(uint64_t), vk::QueryResultFlagBits::e64);
		if (result == vk::Result::eSuccess) {
			const double now = Seconds(p);
			if (p.window_first_s < 0) {
				p.window_first_s = now;
			}
			p.window_last_s = now;
			p.buffers++;
			p.overflows += chunk.overflow ? 1 : 0;
			const auto& ts = p.scratch;
			if (ts[1] > ts[0]) {
				p.span_ns += static_cast<double>(ts[1] - ts[0]) * p.period;
			}
			for (const auto& r: chunk.records) {
				if (!r.ended) {
					continue;
				}
				const uint64_t b = ts[r.query - chunk.first];
				const uint64_t e = ts[r.query - chunk.first + 1];
				const double   ns = e > b ? static_cast<double>(e - b) * p.period : 0.0;
				// For dispatches, "line" 1 marks guest async-compute work.
				const uint32_t line = r.op == GpuCheckpointOp::Copy ? r.arg0 : r.line;
				auto& s = p.stats[StatsKey(r.op, r.key, line)];
				s.op    = r.op;
				s.key   = r.key;
				s.line  = line;
				s.calls++;
				s.ns += ns;
				s.arg0 += r.op == GpuCheckpointOp::Copy ? 0 : r.arg0;
				s.arg1 += r.arg1;
				p.marked_ns += ns;
			}
		}
		p.free_chunks.push_back(chunk.first / CHUNK_QUERIES);
		p.pending.pop_front();
	}
	const auto now = Clock::now();
	if (p.window_first_s >= 0 && now - p.last_report > std::chrono::seconds(5) &&
	    !p.final_written) {
		p.last_report = now;
		WriteReport(p);
		if (!InWindow(p)) {
			p.final_written = true;
		}
	}
}

Chunk* Find(Profiler& p, CmdRef command) {
	auto it = p.active.find(command.Id());
	return it == p.active.end() ? nullptr : &it->second;
}

} // namespace

bool GpuProfileEnabled() {
	auto& p = P();
	std::call_once(p.init_once, [&p] { ParseEnv(p); });
	return p.enabled.load(std::memory_order_relaxed);
}

void GpuProfileBeginCommandBuffer(CommandScheduler& scheduler, CmdRef command) {
	if (!GpuProfileEnabled()) {
		return;
	}
	auto& p = P();
	std::scoped_lock lock(p.mutex);
	if (p.pool == nullptr) {
		p.graphics = &scheduler.Graphics();
		p.period   = p.graphics->physical_device_properties.limits.timestampPeriod;
		const vk::QueryPoolCreateInfo info {.queryType  = vk::QueryType::eTimestamp,
		                                    .queryCount = CHUNK_QUERIES * CHUNK_COUNT};
		if (p.graphics->device.createQueryPool(&info, nullptr, &p.pool) != vk::Result::eSuccess) {
			p.enabled = false;
			return;
		}
		for (uint32_t i = CHUNK_COUNT; i-- > 0;) {
			p.free_chunks.push_back(i);
		}
		p.last_report = Clock::now();
	}
	Collect(p);
	if (!InWindow(p) || p.free_chunks.empty()) {
		p.no_chunk += InWindow(p) ? 1 : 0;
		return;
	}
	const uint32_t index = p.free_chunks.back();
	p.free_chunks.pop_back();
	Chunk chunk;
	chunk.scheduler = &scheduler;
	chunk.first     = index * CHUNK_QUERIES;
	chunk.next      = chunk.first + 2;
	command.resetQueryPool(p.pool, chunk.first, CHUNK_QUERIES);
	command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, p.pool, chunk.first);
	p.active[command.Id()] = std::move(chunk);
}

void GpuProfileEndCommandBuffer(CmdRef command, uint64_t tick) {
	if (!GpuProfileEnabled()) {
		return;
	}
	auto& p = P();
	std::scoped_lock lock(p.mutex);
	auto it = p.active.find(command.Id());
	if (it == p.active.end()) {
		return;
	}
	auto& chunk = it->second;
	// Commands left open (should not happen) are ignored.
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, p.pool, chunk.first + 1);
	chunk.tick = tick;
	chunk.open.clear();
	// Queries never written stay unavailable; only read the range in use.
	p.pending.push_back(std::move(chunk));
	p.active.erase(it);
}

void GpuProfileBegin(CmdRef command, GpuCheckpointOp op, uint64_t key, uint32_t arg0,
                     uint32_t arg1) {
	if (!GpuProfileEnabled()) {
		return;
	}
	auto& p = P();
	std::scoped_lock lock(p.mutex);
	auto* chunk = Find(p, command);
	if (chunk == nullptr) {
		return;
	}
	if (chunk->next + 2 > chunk->first + CHUNK_QUERIES) {
		chunk->overflow = true;
		chunk->open.push_back(SIZE_MAX);
		return;
	}
	const uint32_t query = chunk->next;
	chunk->next += 2;
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, p.pool, query);
	chunk->open.push_back(chunk->records.size());
	const uint32_t async = (op == GpuCheckpointOp::Dispatch || op == GpuCheckpointOp::DispatchIndirect) &&
	                               t_async_compute
	                           ? 1u
	                           : 0u;
	t_async_compute = false;
	chunk->records.push_back({op, key, arg0, arg1, query, async, false});
}

void GpuProfileNoteAsyncCompute(bool async_compute) {
	t_async_compute = async_compute;
}

namespace {

const std::vector<uint64_t>& TraceHashes() {
	static const std::vector<uint64_t> hashes = [] {
		std::vector<uint64_t> list;
		const char*           value = std::getenv("KYTY_TRACE_AFTER");
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
	return hashes;
}

thread_local uint32_t t_trace_remaining  = 0;
thread_local uint32_t t_trace_dispatches = 0;
thread_local uint64_t t_trace_hash       = 0;

} // namespace

void GpuTraceDispatch(uint64_t shader_hash, uint32_t groups_x, uint32_t groups_y,
                      uint32_t groups_z) {
	const auto& hashes = TraceHashes();
	if (t_trace_remaining != 0) {
		std::printf("  trace 0x%016" PRIx64 ": next dispatch shader=0x%016" PRIx64
		            " groups=%u,%u,%u\n",
		            t_trace_hash, shader_hash, groups_x, groups_y, groups_z);
	}
	const auto found = std::find(hashes.begin(), hashes.end(), shader_hash);
	if (found == hashes.end()) {
		return;
	}
	// Per traced shader: skip the first 200 dispatches (loading), then trace 40.
	static thread_local std::unordered_map<uint64_t, uint32_t> seen;
	const uint32_t ordinal = ++seen[shader_hash];
	if (ordinal <= 200 || ordinal > 240) {
		return;
	}
	t_trace_dispatches = ordinal - 200;
	t_trace_remaining  = 24;
	t_trace_hash       = shader_hash;
	std::printf("trace after dispatch 0x%016" PRIx64 " #%u groups=%u,%u,%u\n", shader_hash,
	            t_trace_dispatches, groups_x, groups_y, groups_z);
}

void GpuTracePacket(uint32_t opcode, const uint32_t* packet, uint32_t size_dw) {
	if (t_trace_remaining == 0) {
		return;
	}
	t_trace_remaining--;
	std::printf("  trace 0x%016" PRIx64 ": op=0x%02x len=%u", t_trace_hash, opcode, size_dw);
	for (uint32_t i = 1; i < size_dw && i < 7; i++) {
		std::printf(" %08x", packet[i]);
	}
	std::printf("\n");
}

void GpuProfileEnd(CmdRef command) {
	if (!GpuProfileEnabled()) {
		return;
	}
	auto& p = P();
	std::scoped_lock lock(p.mutex);
	auto* chunk = Find(p, command);
	if (chunk == nullptr || chunk->open.empty()) {
		return;
	}
	const size_t index = chunk->open.back();
	chunk->open.pop_back();
	if (index == SIZE_MAX) {
		return;
	}
	auto& record = chunk->records[index];
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, p.pool, record.query + 1);
	record.ended = true;
}

} // namespace Libs::Graphics
