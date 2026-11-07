#include "graphics/host_gpu/renderer/pipeline/prewalk.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/sliceWriteLog.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Prewalk {

namespace IR = ShaderRecompiler::IR;
using Common::DebugCounters::Counter;
using Program = PipelineCache::Program;

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PREWALK");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// KYTY_PREWALK_DIAGNOSE=1: print the first mispredicted user data.
static bool Diagnose() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PREWALK_DIAGNOSE");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

static bool VerifyEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PREWALK_VERIFY");
		return value != nullptr && value[0] == '1';
	}();
	return enabled;
}

namespace {

// Programs the GPU thread looked up last for a stage and code address, with how it built their
// user data (count and the merged-stage s0:s7 prefix).
struct Published {
	Program*                program         = nullptr;
	uint32_t                user_data_count = 0;
	std::array<uint32_t, 8> prefix {};
	bool                    eligible = false;

	bool operator==(const Published&) const = default;
};

uint64_t Key(Stage stage, uint64_t base) {
	return base | (static_cast<uint64_t>(stage) << 60u);
}

std::shared_mutex                      g_published_mutex;
std::unordered_map<uint64_t, Published> g_published;      // shared with the helper
std::unordered_map<uint64_t, Published> g_published_gpu;  // GPU thread's copy (no lock)

// Plans whose materialization reads only guest memory through the walker and recorded
// specialization reads, and touches no GPU-thread state.
bool Eligible(const Program& program) {
	const auto& plan = program.resource_plan;
	return !program.skip_dispatch && program.call_source == nullptr &&
	       plan.resource_tracking_complete && plan.srt_code != nullptr && plan.source_reads.empty() &&
	       plan.selector_keys.empty() &&
	       std::ranges::none_of(plan.descriptor_sources, [](const auto& source) {
		       return source.indirect_descriptor.has_value();
	       });
}

bool ReadGuest(void*, uint64_t address, std::span<uint32_t> values) {
	return !values.empty() &&
	       LibKernel::Memory::TryReadBacking(address, values.data(), values.size_bytes());
}

constexpr uint32_t MaxJobs = 512;

struct Result {
	// 0: being materialized, 1: ready, 2: failed or skipped.
	std::atomic<uint8_t>                       state {0};
	Stage                                      stage           = Stage::Vertex;
	uint64_t                                   base            = 0;
	uint32_t                                   user_data_count = 0;
	std::array<uint32_t, 40>                   user_data {};
	Program*                                   program = nullptr;
	IR::ResourceSnapshot                       snapshot;
	IR::ResourceSpecialization                 specialization;
	std::vector<std::pair<uint64_t, uint64_t>> reads; // walker captures
};

struct Batch {
	// 0 while the GPU thread refills the batch; the helper drops a batch whose generation changed.
	std::atomic<uint64_t>                  generation {0};
	HW::Shader                             shaders;
	std::vector<std::span<const uint32_t>> pending; // back() runs first
	std::atomic<uint32_t>                  produced {0}; // results below hold their keys
	std::atomic<uint32_t>                  consumed {0}; // the GPU thread will not take these
	std::atomic<uint32_t>                  lookups {0};  // GPU-thread materializations so far
	std::array<Result, MaxJobs>            results;
};

class Helper {
public:
	explicit Helper(RenderContext& context)
	    : m_context(context), m_shadow(std::make_unique<CommandProcessor>(context, 0)) {
		for (auto& batch: m_batches) batch = std::make_unique<Batch>();
		m_thread = std::thread([this] { Run(); });
		m_thread.detach(); // lives as long as the process
	}

	// GPU thread: the batch for a new slice, empty and no longer read by the helper.
	Batch& Acquire() {
		auto& batch = *m_batches[m_next];
		m_next      = (m_next + 1u) % m_batches.size();
		batch.generation.store(0, std::memory_order_seq_cst);
		while (m_working.load(std::memory_order_seq_cst) == &batch) _mm_pause();
		const auto produced = batch.produced.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < produced && i < MaxJobs; ++i) {
			batch.results[i].state.store(0, std::memory_order_relaxed);
		}
		batch.produced.store(0, std::memory_order_relaxed);
		batch.consumed.store(0, std::memory_order_relaxed);
		batch.lookups.store(0, std::memory_order_relaxed);
		return batch;
	}

	void Post(Batch& batch) {
		batch.generation.store(++m_generation, std::memory_order_seq_cst);
		m_posted.store(&batch, std::memory_order_seq_cst);
		m_post_count.fetch_add(1, std::memory_order_seq_cst);
		m_post_count.notify_one();
	}

private:
	void Run() {
		IR::SetSrtWalkerHelperThread(true);
		uint64_t seen = 0;
		for (;;) {
			// Slices follow each other within microseconds: poll a while before sleeping, whose
			// wake-up would cost a good part of the next slice.
			// About 30 us: a sleeping helper's wake-up costs a good part of a slice, a long spin
			// takes a core from the game's own threads.
			for (const auto until = __rdtsc() + 150000u;
			     __rdtsc() < until && m_post_count.load(std::memory_order_acquire) == seen;) {
				_mm_pause();
			}
			m_post_count.wait(seen, std::memory_order_seq_cst);
			seen       = m_post_count.load(std::memory_order_seq_cst);
			auto* batch = m_posted.load(std::memory_order_seq_cst);
			if (batch == nullptr) continue;
			m_working.store(batch, std::memory_order_seq_cst);
			const auto generation = batch->generation.load(std::memory_order_seq_cst);
			if (m_posted.load(std::memory_order_seq_cst) == batch && generation != 0) {
				Common::DebugCounters::ScopedTimer timer(Counter::PrewalkHelperBatches,
				                                         Counter::PrewalkHelperNs);
				Process(*batch, generation);
			}
			m_working.store(nullptr, std::memory_order_seq_cst);
		}
	}

	// Memory written by packets earlier in this scan (labels for GPU-internal fences).
	void NoteScanWrite(uint64_t address, uint64_t size) {
		if (address != 0 && size != 0 && m_scan_writes.size() < 256u) m_scan_writes.emplace_back(address, size);
	}
	[[nodiscard]] bool WrittenByScan(uint64_t address, uint64_t size) const {
		return std::ranges::any_of(m_scan_writes, [&](const auto& write) {
			return write.first < address + size && address < write.first + write.second;
		});
	}

	static void Stop(uint32_t opcode, uint32_t header, int line) {
		Common::DebugCounters::Add(Counter::PrewalkScanStops);
		static std::atomic<uint32_t> printed {0};
		if (printed.fetch_add(1, std::memory_order_relaxed) < 40u) {
			std::printf("Prewalk scan stop: op=0x%02x header=0x%08x line=%d\n", opcode, header, line);
			std::fflush(stdout);
		}
	}

	bool Current(const Batch& batch, uint64_t generation) const {
		return batch.generation.load(std::memory_order_acquire) == generation;
	}

	// Replays the slice's SH register packets and materializes each draw's and dispatch's programs.
	void Process(Batch& batch, uint64_t generation) {
		m_shadow->GetShCtx() = batch.shaders;
		m_scan_writes.clear();
		auto stack = batch.pending;
		while (!stack.empty()) {
			if (!Current(batch, generation) || batch.produced.load(std::memory_order_relaxed) >= MaxJobs) {
				return;
			}
			auto& cursor = stack.back();
			if (cursor.empty()) {
				stack.pop_back();
				continue;
			}
			const uint32_t header = cursor[0];
			if (header == 0x80000000u || Pm4::PacketSizeDw(header) == 1u) {
				cursor = cursor.subspan(1);
				continue;
			}
			const uint32_t length = KYTY_PM4_LEN(header);
			if ((header >> 30u) != 3u || length < 2u || length > cursor.size()) {
				Common::DebugCounters::Add(Counter::PrewalkScanLost);
				return;
			}
			const uint32_t  opcode    = (header >> 8u) & 0xffu;
			const uint32_t* body      = cursor.data() + 1;
			const auto      remaining = static_cast<uint32_t>(cursor.size());
			switch (opcode) {
				case Pm4::IT_SET_SH_REG:
					(void)g_cp_op_func[opcode](*m_shadow, header & ~1u, body, remaining, remaining);
					break;
				case Pm4::IT_SET_SH_REG_INDIRECT: {
					if (length != 5u) return;
					const uint64_t table = (uint64_t {body[0]} & 0xfffffffcu) | (uint64_t {body[1]} << 32u);
					const uint32_t count = body[3] & 0x3fffu;
					if (count != 0) {
						// Off the GPU thread TryReadBufferBacking refuses GPU-visible memory (on it, it
						// first downloads GPU-written bytes). Read the backing directly when no page
						// holds GPU-written bytes, then hand the handler that copy: its own backing
						// read of a host address fails and it falls back to reading the pointer.
						const uint64_t bytes = uint64_t {count} * 8u;
						m_table.resize(size_t {count} * 2u);
						if (table == 0 ||
						    m_context.GetBufferCache().IsRegionGpuModified(table & ~uint64_t {0xfff},
						                                                   ((table + bytes + 0xfffu) & ~uint64_t {0xfff}) -
						                                                       (table & ~uint64_t {0xfff})) ||
						    !LibKernel::Memory::TryReadBacking(table, m_table.data(), bytes)) {
							Stop(opcode, header, __LINE__);
							return;
						}
						const auto copy = reinterpret_cast<uint64_t>(m_table.data());
						const std::array<uint32_t, 4> local {static_cast<uint32_t>(copy),
						                                     static_cast<uint32_t>(copy >> 32u), body[2], body[3]};
						(void)g_cp_op_func[opcode](*m_shadow, header & ~1u, local.data(), remaining, remaining);
					}
					break;
				}
				case Pm4::IT_INDIRECT_BUFFER: {
					if (length != 4u) { // a branch
						Stop(opcode, header, __LINE__);
						return;
					}
					const uint64_t address = uint64_t {body[0]} | (uint64_t {body[1]} << 32u);
					const uint32_t count   = body[2] & 0xfffffu;
					const bool     chain   = (body[2] & (1u << 20u)) != 0;
					cursor                 = cursor.subspan(length);
					if (count == 0) continue;
					if (!m_context.IsMapped(address, uint64_t {count} * 4u)) {
						Stop(opcode, header, __LINE__);
						return;
					}
					const std::span<const uint32_t> buffer {reinterpret_cast<const uint32_t*>(address), count};
					if (chain) {
						stack.back() = buffer;
					} else {
						stack.push_back(buffer);
					}
					continue;
				}
				case Pm4::IT_DRAW_INDEX_2:
				case Pm4::IT_DRAW_INDEX_OFFSET_2:
				case Pm4::IT_DRAW_INDEX_AUTO:
				case Pm4::IT_DRAW_INDIRECT:
				case Pm4::IT_DRAW_INDEX_INDIRECT:
				case Pm4::IT_DRAW_INDIRECT_MULTI:
				case Pm4::IT_DRAW_INDEX_INDIRECT_MULTI: {
					const auto& sh = m_shadow->GetShCtx();
					const auto& vs = sh.GetVs();
					const auto& ps = sh.GetPs();
					Emit(batch, generation, Stage::Vertex, vs.es_regs.data_addr, vs.gs_user_sgpr.value,
					     vs.gs_regs.rsrc2.user_sgpr);
					Emit(batch, generation, Stage::Pixel, ps.ps_regs.data_addr, ps.ps_user_sgpr.value,
					     ps.ps_regs.rsrc2.user_sgpr);
					break;
				}
				case Pm4::IT_DISPATCH_DIRECT: {
					const auto& cs = m_shadow->GetShCtx().GetCs();
					Emit(batch, generation, Stage::Compute, cs.cs_regs.data_addr, cs.cs_user_sgpr.value,
					     cs.cs_regs.user_sgpr);
					break;
				}
				// The GPU waits for something another agent (often the CPU) signals; memory read after
				// it may be written meanwhile without the slice ending. A wait already satisfied now
				// is safe to pass: whatever its signaller wrote before the signal is visible already
				// (x86 stores are seen in order), and later CPU writes are not the guest's to rely on.
				case Pm4::IT_WAIT_REG_MEM:
				case Pm4::IT_WAIT_REG_MEM_64: {
					const bool     wide  = opcode == Pm4::IT_WAIT_REG_MEM_64;
					const uint32_t value_dw = wide ? 2u : 1u;
					if (length != 5u + value_dw * 2u || (body[0] & 0x10u) == 0) {
						Stop(opcode, header, __LINE__);
						return;
					}
					const uint32_t function = body[0] & 0x7u;
					const uint64_t address  = (uint64_t {body[1]} & (wide ? ~uint64_t {7} : ~uint64_t {3})) |
					                         (uint64_t {body[2] & 0x3ffffu} << 32u);
					const uint64_t ref  = wide ? (uint64_t {body[3]} | (uint64_t {body[4]} << 32u)) : body[3];
					const uint64_t mask = wide ? (uint64_t {body[5]} | (uint64_t {body[6]} << 32u)) : body[4];
					uint64_t       current = 0;
					const bool     satisfied =
					    function <= 6u && address != 0 &&
					    !m_context.GetBufferCache().IsRegionGpuModified(address & ~uint64_t {0xfff}, 4096u) &&
					    LibKernel::Memory::TryReadBacking(address, &current, wide ? 8u : 4u) &&
					    TestWaitRegMemValue(current, ref, mask, function);
					if (satisfied) break;
					// Unsatisfied now. The wait suspends (ends the slice) unless its label is set when
					// the GPU thread gets there. A label an earlier packet of this command buffer
					// writes is a GPU-internal fence: passing it brings in no CPU writes. If it does
					// suspend, this batch is dropped with the slice.
					if (function <= 6u && WrittenByScan(address, wide ? 8u : 4u)) {
						Common::DebugCounters::Add(Counter::PrewalkScanFences);
						break;
					}
					Common::DebugCounters::Add(Counter::PrewalkScanWaits);
					return;
				}
				case Pm4::IT_WRITE_DATA:
					if (length > 4u) {
						NoteScanWrite(uint64_t {body[1]} | (uint64_t {body[2]} << 32u), uint64_t {length - 4u} * 4u);
					}
					break;
				case Pm4::IT_RELEASE_MEM:
					if (length == 8u) {
						NoteScanWrite((uint64_t {body[2]} & ~uint64_t {3}) | (uint64_t {body[3]} << 32u), 8u);
					}
					break;
				case Pm4::IT_COPY_DATA:
					if (length == 6u) NoteScanWrite(uint64_t {body[3]} | (uint64_t {body[4]} << 32u), 8u);
					break;
				case Pm4::IT_DMA_DATA:
					if (length == 7u) {
						NoteScanWrite(uint64_t {body[3]} | (uint64_t {body[4]} << 32u), body[5] & 0x03ffffffu);
					}
					break;
				case Pm4::IT_EVENT_WRITE:
					if (length >= 4u) NoteScanWrite(uint64_t {body[1]} | (uint64_t {body[2]} << 32u), 64u);
					break;
				case Pm4::IT_MEM_SEMAPHORE:
					if (length == 4u && (body[2] >> 29u) == 6u) break; // a signal
					Stop(opcode, header, __LINE__);
					return;
				case Pm4::IT_COND_EXEC:
				case Pm4::IT_REWIND: Stop(opcode, header, __LINE__); return;
				case Pm4::IT_NOP:
					if (KYTY_PM4_R(header) == Pm4::R_DISPATCH_RESET) {
						m_shadow->Reset(); // as CpOpDispatchReset: registers only
						break;
					}
					if (KYTY_PM4_R(header) == Pm4::R_WAIT_FLIP_DONE) {
						Stop(opcode, header, __LINE__);
						return;
					}
					break;
				default: break;
			}
			cursor = cursor.subspan(length);
		}
	}

	void Emit(Batch& batch, uint64_t generation, Stage stage, uint64_t base, const uint32_t* sgprs,
	          uint32_t count) {
		Common::DebugCounters::Add(Counter::PrewalkEmits);
		if (base == 0 || count > HW::UserSgprInfo::SGPRS_MAX) return;
		Published published;
		{
			std::shared_lock lock(g_published_mutex);
			const auto found = g_published.find(Key(stage, base));
			if (found == g_published.end()) {
				Common::DebugCounters::Add(Counter::PrewalkEmitUnpublished);
				return;
			}
			published = found->second;
		}
		if (!published.eligible) {
			Common::DebugCounters::Add(Counter::PrewalkEmitIneligible);
			return;
		}
		uint32_t prefix = 0;
		if (published.user_data_count == count + 8u) {
			prefix = 8u;
		} else if (published.user_data_count != count) {
			Common::DebugCounters::Add(Counter::PrewalkEmitCount);
			return;
		}
		const auto index = batch.produced.load(std::memory_order_relaxed);
		if (index >= MaxJobs) return;
		auto& result           = batch.results[index];
		result.stage           = stage;
		result.base            = base;
		result.program         = published.program;
		result.user_data_count = published.user_data_count;
		std::copy_n(published.prefix.begin(), prefix, result.user_data.begin());
		std::copy_n(sgprs, count, result.user_data.begin() + prefix);
		result.state.store(0, std::memory_order_relaxed);
		batch.produced.store(index + 1u, std::memory_order_release);
		Common::DebugCounters::Add(Counter::PrewalkJobs);
		// Behind the GPU thread (it already did more lookups than this job's position): skip.
		if (index < batch.consumed.load(std::memory_order_acquire) ||
		    index + 4u < batch.lookups.load(std::memory_order_acquire) || !Current(batch, generation)) {
			Common::DebugCounters::Add(Counter::PrewalkHelperSkipped);
			result.state.store(2, std::memory_order_release);
			return;
		}
		result.state.store(Materialize(result) ? 1 : 2, std::memory_order_release);
	}

	bool Materialize(Result& result) {
		const auto& plan = result.program->resource_plan;
		m_scratch.assign(plan.srt_scratch.size(), 0u);
		result.reads.clear();
		IR::SetSrtScratchOverride(m_scratch.data(), &result.reads);
		const IR::SrtRuntime runtime {
		    .user_data                  = std::span(result.user_data).first(result.user_data_count),
		    .shader_base                = result.base,
		    .read_specialization_memory = ReadGuest,
		};
		(void)IR::TakeSrtWalkerHelperAborted();
		bool ok = IR::MaterializeResources(plan, runtime, result.snapshot, result.specialization);
		ok &= !IR::TakeSrtWalkerHelperAborted();
		IR::SetSrtScratchOverride(nullptr, nullptr);
		if (!ok) {
			Common::DebugCounters::Add(Counter::PrewalkHelperFailed);
			return false;
		}
		// Bytes the GPU wrote and the guest memory does not hold yet may be published by another
		// thread before the GPU thread reads them; such a result cannot be trusted. Walker loads
		// prove their pages clean (GPU-written pages are read-protected; a load there faults and
		// aborts the job), so only the non-faulting post-walk reads need the tracker (whose region
		// locks the game threads' fault handlers also take).
		auto& cache = m_context.GetBufferCache();
		m_walked_pages.clear();
		for (const auto& [address, size]: result.reads) m_walked_pages.push_back(address >> 12u);
		std::ranges::sort(m_walked_pages);
		uint64_t   last_page = UINT64_MAX;
		const auto clean     = [&](const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
			for (const auto& [address, size]: ranges) {
				if (size == 0) continue;
				for (uint64_t page = address >> 12u; page <= (address + size - 1u) >> 12u; ++page) {
					if (page == last_page || std::ranges::binary_search(m_walked_pages, page)) continue;
					last_page = page;
					if (cache.IsRegionGpuModified(page << 12u, 4096u)) return false;
				}
			}
			return true;
		};
		if (!clean(result.snapshot.specialization_reads)) {
			Common::DebugCounters::Add(Counter::PrewalkHelperGpuDirty);
			return false;
		}
		return true;
	}

	RenderContext&                         m_context;
	std::unique_ptr<CommandProcessor>      m_shadow;
	std::array<std::unique_ptr<Batch>, 3>  m_batches;
	uint32_t                               m_next       = 0;
	uint64_t                               m_generation = 0;
	std::atomic<Batch*>                    m_posted {nullptr};
	std::atomic<Batch*>                    m_working {nullptr};
	std::atomic<uint64_t>                  m_post_count {0};
	std::vector<uint64_t>                  m_scratch;
	std::vector<uint32_t>                  m_table;
	std::vector<std::pair<uint64_t, uint64_t>> m_scan_writes;
	std::vector<uint64_t>                  m_walked_pages;
	std::thread                            m_thread;
};

Helper* g_helper  = nullptr;
Batch*  g_current = nullptr; // GPU thread: this slice's batch

bool SameSnapshot(const IR::ResourceSnapshot& a, const IR::ResourceSnapshot& b) {
	return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
	       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data &&
	       a.specialization_reads == b.specialization_reads;
}

} // namespace

void Publish(Stage stage, Program& program, const ShaderParams& params) {
	if (!Enabled()) return;
	Published value {.program = &program, .user_data_count = params.user_data_count,
	                 .eligible = Eligible(program)};
	std::copy_n(params.user_data.begin(), value.prefix.size(), value.prefix.begin());
	const auto key   = Key(stage, params.Base());
	auto&      local = g_published_gpu[key];
	if (local == value) return;
	local = value;
	std::unique_lock lock(g_published_mutex);
	g_published[key] = value;
}

void BeginSlice(CommandProcessor& cp, RenderContext& context,
                std::vector<std::span<const uint32_t>> pending) {
	if (!Enabled()) return;
	// Follows KYTY_AB_PERIOD: in "off" seconds the helper gets nothing and nothing is taken.
	if (!Common::DebugCounters::AbVariantFor("prewalk")) {
		g_current = nullptr;
		return;
	}
	if (g_helper == nullptr) g_helper = new Helper(context);
	auto& batch   = g_helper->Acquire();
	batch.shaders = cp.GetShCtx();
	batch.pending = std::move(pending);
	g_current     = &batch;
	g_helper->Post(batch);
}

bool Take(Program& program, const IR::SrtRuntime& runtime) {
	if (!Enabled() || g_current == nullptr || runtime.indirect_args_address != 0u) return false;
	auto&      batch    = *g_current;
	const auto produced = batch.produced.load(std::memory_order_acquire);
	auto       index    = batch.consumed.load(std::memory_order_relaxed);
	batch.lookups.fetch_add(1, std::memory_order_release);
	constexpr uint32_t Window = 24;
	bool same_program = false;
	const bool empty = index >= produced;
	for (const auto end = std::min(produced, index + Window); index < end; ++index) {
		auto& result = batch.results[index];
		if (result.program != &program) continue;
		same_program = true;
		if (result.base != runtime.shader_base || result.user_data_count != runtime.user_data.size() ||
		    !std::equal(runtime.user_data.begin(), runtime.user_data.end(), result.user_data.begin())) {
			if (Diagnose()) {
				static std::atomic<uint32_t> printed {0};
				if (printed.fetch_add(1, std::memory_order_relaxed) < 30u) {
					std::printf("Prewalk user data differ: stage=%u count %u/%zu", static_cast<uint32_t>(result.stage),
					            result.user_data_count, runtime.user_data.size());
					for (size_t i = 0; i < runtime.user_data.size() && i < 16; ++i) {
						if (runtime.user_data[i] != result.user_data[i])
							std::printf(" [%zu] %08x/%08x", i, result.user_data[i], runtime.user_data[i]);
					}
					std::printf("\n");
					std::fflush(stdout);
				}
			}
			continue;
		}
		batch.consumed.store(index + 1u, std::memory_order_release);
		const auto state = result.state.load(std::memory_order_acquire);
		if (state != 1) {
			Common::DebugCounters::Add(state == 0 ? Counter::PrewalkNotReady : Counter::PrewalkFailed);
			return false;
		}
		const auto written = [](const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
			return std::ranges::any_of(ranges, [](const auto& range) {
				return range.second != 0 && SliceWriteLog::RangeWritten(range.first, range.second);
			});
		};
		if (written(result.reads) || written(result.snapshot.specialization_reads)) {
			Common::DebugCounters::Add(Counter::PrewalkRejected);
			return false;
		}
		std::swap(program.resources, result.snapshot);
		std::swap(program.specialization, result.specialization);
		Common::DebugCounters::Add(Counter::PrewalkTaken);
		if (VerifyEnabled()) {
			IR::ResourceSnapshot       snapshot;
			IR::ResourceSpecialization specialization;
			const bool again = IR::MaterializeResources(program.resource_plan, runtime, snapshot,
			                                            specialization);
			if (!again || !(specialization == program.specialization) ||
			    !SameSnapshot(snapshot, program.resources)) {
				Common::DebugCounters::Add(Counter::PrewalkVerifyMismatches);
				static std::atomic<uint32_t> printed {0};
				if (printed.fetch_add(1, std::memory_order_relaxed) < 20u) {
					std::printf("Prewalk MISMATCH stage=%u base=0x%016llx again=%d spec=%d buffers=%d "
					            "images=%d samplers=%d flat=%d user=%d reads=%d\n",
					            static_cast<uint32_t>(result.stage),
					            static_cast<unsigned long long>(runtime.shader_base), again ? 1 : 0,
					            specialization == program.specialization ? 1 : 0,
					            snapshot.buffers == program.resources.buffers ? 1 : 0,
					            snapshot.images == program.resources.images ? 1 : 0,
					            snapshot.samplers == program.resources.samplers ? 1 : 0,
					            snapshot.flattened_srt == program.resources.flattened_srt ? 1 : 0,
					            snapshot.user_data == program.resources.user_data ? 1 : 0,
					            snapshot.specialization_reads == program.resources.specialization_reads ? 1 : 0);
					std::fflush(stdout);
				}
				if (again) {
					program.resources      = std::move(snapshot);
					program.specialization = std::move(specialization);
				} else {
					return false;
				}
			}
		}
		return true;
	}
	Common::DebugCounters::Add(Counter::PrewalkNoMatch);
	Common::DebugCounters::Add(empty          ? Counter::PrewalkNoMatchEmpty
	                           : same_program ? Counter::PrewalkNoMatchUserData
	                                          : Counter::PrewalkNoMatchProgram);
	return false;
}

} // namespace Libs::Graphics::Prewalk
