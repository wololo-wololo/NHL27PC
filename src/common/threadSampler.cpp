#include "common/threadSampler.h"

#include "common/common.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <dbghelp.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace Common::ThreadSampler {

namespace {

constexpr size_t MAX_FRAMES = 48;

struct Sample {
	uint32_t                          count = 0;
	std::array<uint64_t, MAX_FRAMES> frames {};
};

// Walks the suspended thread's stack with the x64 unwind tables. Code without unwind data
// (the guest and JIT code) ends the walk. Uses no heap memory while the thread is suspended.
uint32_t Walk(CONTEXT context, std::array<uint64_t, MAX_FRAMES>& frames) {
	uint32_t count = 0;
	while (count < MAX_FRAMES && context.Rip != 0) {
		frames[count++] = context.Rip;
		DWORD64 image_base = 0;
		auto*   function   = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
		if (function == nullptr) {
			// Only a leaf function at the top may lack unwind data; it keeps its return address
			// at the stack pointer.
			if (count != 1) {
				break;
			}
			context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
			context.Rsp += sizeof(DWORD64);
			continue;
		}
		void*   handler_data = nullptr;
		DWORD64 frame        = 0;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
		                 &handler_data, &frame, nullptr);
	}
	return count;
}

std::string Symbol(HANDLE process, uint64_t address, std::unordered_map<uint64_t, std::string>& cache) {
	if (auto it = cache.find(address); it != cache.end()) {
		return it->second;
	}
	alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512] {};
	auto* symbol         = reinterpret_cast<SYMBOL_INFO*>(storage);
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen   = 511;
	DWORD64     displacement = 0;
	std::string name;
	if (SymFromAddr(process, address, &displacement, symbol) != 0) {
		name.assign(symbol->Name, symbol->NameLen);
	} else {
		char text[32];
		std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
		name = text;
	}
	return cache.emplace(address, std::move(name)).first->second;
}

void Report(const std::vector<Sample>& samples, const char* name) {
	const HANDLE process = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(process, nullptr, TRUE);
	std::unordered_map<uint64_t, std::string> cache;
	std::unordered_map<std::string, uint64_t> exclusive;
	std::unordered_map<std::string, uint64_t> inclusive;
	// The faulting function and its callers, for samples inside guest-memory fault handling.
	std::unordered_map<std::string, uint64_t> fault_sites;
	uint64_t                                  total = 0;
	for (const auto& sample: samples) {
		if (sample.count == 0) {
			continue;
		}
		total++;
		for (uint32_t i = 0; i + 1 < sample.count; i++) {
			if (Symbol(process, sample.frames[i], cache) != "KiUserExceptionDispatcher") {
				continue;
			}
			std::string site;
			for (uint32_t j = i + 1; j < sample.count && j < i + 4; j++) {
				site += (site.empty() ? "" : " <- ") + Symbol(process, sample.frames[j], cache);
			}
			fault_sites[site]++;
			break;
		}
		exclusive[Symbol(process, sample.frames[0], cache)]++;
		std::unordered_set<std::string> seen;
		for (uint32_t i = 0; i < sample.count; i++) {
			auto symbol = Symbol(process, sample.frames[i], cache);
			if (seen.insert(symbol).second) {
				inclusive[symbol]++;
			}
		}
	}
	SymCleanup(process);
	const auto sorted = [](const std::unordered_map<std::string, uint64_t>& map) {
		std::vector<std::pair<std::string, uint64_t>> rows(map.begin(), map.end());
		std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second > b.second; });
		return rows;
	};
	FILE* file = std::fopen("thread-samples.txt", "w");
	if (file == nullptr) {
		return;
	}
	std::fprintf(file, "thread %s: %llu samples\n\n== exclusive ==\n", name,
	             static_cast<unsigned long long>(total));
	const auto print = [&](const auto& rows) {
		for (size_t i = 0; i < rows.size() && i < 80; i++) {
			std::fprintf(file, "%6.2f%%  %s\n", 100.0 * static_cast<double>(rows[i].second) / static_cast<double>(std::max<uint64_t>(total, 1)),
			             rows[i].first.c_str());
		}
	};
	print(sorted(exclusive));
	std::fprintf(file, "\n== inclusive ==\n");
	print(sorted(inclusive));
	std::fprintf(file, "\n== fault sites ==\n");
	print(sorted(fault_sites));
	std::fclose(file);
	std::printf("ThreadSampler: wrote thread-samples.txt (%llu samples)\n",
	            static_cast<unsigned long long>(total));
	std::fflush(stdout);
}

} // namespace

void RegisterCurrentThreadFromEnvironment(const char* name) {
	const char* value = std::getenv("KYTY_SAMPLE_GPU");
	if (value == nullptr || *value == '\0') {
		return;
	}
	double delay    = 0.0;
	double duration = 20.0;
	if (std::sscanf(value, "%lf,%lf", &delay, &duration) < 1) {
		return;
	}
	HANDLE thread = nullptr;
	if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
	                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
	                    FALSE, 0) == 0) {
		return;
	}
	std::thread([thread, delay, duration, name = std::string(name)] {
		std::this_thread::sleep_for(std::chrono::duration<double>(delay));
		std::vector<Sample> samples(static_cast<size_t>(duration * 1100.0) + 16);
		size_t              used  = 0;
		const auto          until = std::chrono::steady_clock::now() +
		                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		                       std::chrono::duration<double>(duration));
		// Unwinding a thread suspended inside ntdll (exception dispatch, function-table updates)
		// reads its half-updated state and crashed the sampler twice; such samples keep only the
		// instruction pointer.
		const auto ntdll = reinterpret_cast<uint64_t>(GetModuleHandleW(L"ntdll.dll"));
		uint64_t   ntdll_size = 0;
		if (ntdll != 0) {
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ntdll);
			const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(ntdll + dos->e_lfanew);
			ntdll_size      = nt->OptionalHeader.SizeOfImage;
		}
		timeBeginPeriod(1);
		while (std::chrono::steady_clock::now() < until && used < samples.size()) {
			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				CONTEXT context {};
				context.ContextFlags = CONTEXT_FULL;
				if (GetThreadContext(thread, &context) != 0) {
					if (context.Rip - ntdll < ntdll_size) {
						samples[used].frames[0] = context.Rip;
						samples[used].count     = 1;
					} else {
						samples[used].count = Walk(context, samples[used].frames);
					}
					used++;
				}
				ResumeThread(thread);
			}
			Sleep(1);
		}
		timeEndPeriod(1);
		samples.resize(used);
		Report(samples, name.c_str());
		CloseHandle(thread);
	}).detach();
}

void PrintCurrentStack(const char* label) {
	static std::mutex mutex;
	std::scoped_lock  lock(mutex);
	static const bool initialized = [] {
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
	}();
	(void)initialized;
	void*      frames[32] {};
	const auto count = CaptureStackBackTrace(1, 32, frames, nullptr);
	std::unordered_map<uint64_t, std::string> cache;
	std::printf("%s (thread %lu):\n", label, GetCurrentThreadId());
	for (USHORT i = 0; i < count; i++) {
		std::printf("    %s\n",
		            Symbol(GetCurrentProcess(), reinterpret_cast<uint64_t>(frames[i]), cache).c_str());
	}
	std::fflush(stdout);
}

} // namespace Common::ThreadSampler

#else

namespace Common::ThreadSampler {
void RegisterCurrentThreadFromEnvironment(const char* /*name*/) {}
void PrintCurrentStack(const char* /*label*/) {}
} // namespace Common::ThreadSampler

#endif
