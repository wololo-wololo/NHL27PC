#include "common/debugCounters.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stop_token>
#include <thread>

namespace Common::DebugCounters {

namespace {

constexpr auto COUNTER_COUNT = static_cast<size_t>(Counter::Count);

constexpr std::array<const char*, COUNTER_COUNT> NAMES {
#define KYTY_DEBUG_COUNTER_NAME(name) #name,
    KYTY_DEBUG_COUNTERS(KYTY_DEBUG_COUNTER_NAME)
#undef KYTY_DEBUG_COUNTER_NAME
};

std::mutex   g_logger_mutex;
std::jthread g_logger;

void LoggerThread(const std::stop_token& stop, FILE* file) {
	using Clock = std::chrono::steady_clock;
	std::array<uint64_t, COUNTER_COUNT> previous {};
	std::array<uint64_t, Detail::OPCODE_SLOTS> previous_calls {};
	std::array<uint64_t, Detail::OPCODE_SLOTS> previous_ns {};
	std::array<uint64_t, Detail::OPCODE_SLOTS> previous_readback {};
	const auto                          origin = Clock::now();
	auto                                last   = origin;
	const char*                         period_text = std::getenv("KYTY_AB_PERIOD");
	const int  ab_period = period_text != nullptr ? std::atoi(period_text) : 0;
	int        ab_tick   = 0;
	while (!stop.stop_requested()) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto   now     = Clock::now();
		const double seconds = std::chrono::duration<double>(now - last).count();
		last                 = now;
		std::fprintf(file, "t=%.1f ab=%d", std::chrono::duration<double>(now - origin).count(),
		             AbVariant() ? 1 : 0);
		if (ab_period > 0 && ++ab_tick % ab_period == 0) {
			Detail::g_ab_variant.store(!AbVariant(), std::memory_order_relaxed);
		}
		for (size_t i = 0; i < COUNTER_COUNT; i++) {
			const auto value = Get(static_cast<Counter>(i));
			if (value != previous[i]) {
				std::fprintf(file, " %s=%.0f", NAMES[i],
				             static_cast<double>(value - previous[i]) / seconds);
			}
			previous[i] = value;
		}
		for (uint32_t op = 0; op < Detail::OPCODE_SLOTS; op++) {
			const auto calls = GetOpcodeCalls(op);
			const auto ns    = GetOpcodeNs(op);
			if (calls != previous_calls[op]) {
				std::fprintf(file, op < 256 ? " op%02X=%.0f/%.0f" : " r%02X=%.0f/%.0f", op & 0xffu,
				             static_cast<double>(calls - previous_calls[op]) / seconds,
				             static_cast<double>(ns - previous_ns[op]) / seconds);
			}
			const auto readback = GetOpcodeReadbackNs(op);
			if (readback != previous_readback[op]) {
				std::fprintf(file, op < 256 ? " rbop%02X=%.0f" : " rbr%02X=%.0f", op & 0xffu,
				             static_cast<double>(readback - previous_readback[op]) / seconds);
			}
			previous_calls[op]     = calls;
			previous_ns[op]        = ns;
			previous_readback[op]  = readback;
		}
		std::fputc('\n', file);
		std::fflush(file);
	}
	std::fclose(file);
}

} // namespace

const char* Name(Counter counter) noexcept {
	const auto index = static_cast<size_t>(counter);
	return index < COUNTER_COUNT ? NAMES[index] : "?";
}

void StartCsvLoggerFromEnvironment() {
	const char* path = std::getenv("KYTY_PERF_CSV");
	if (path == nullptr || *path == '\0') {
		return;
	}
	std::scoped_lock lock(g_logger_mutex);
	if (g_logger.joinable()) {
		return;
	}
	FILE* file = std::fopen(path, "a");
	if (file == nullptr) {
		return;
	}
	std::fputs("# kyty perf log: counter=rate/s, opXX=calls/s/ns-per-s\n", file);
	SetEnabled(true);
	g_logger = std::jthread([file](const std::stop_token& stop) { LoggerThread(stop, file); });
}

void StopCsvLogger() {
	std::scoped_lock lock(g_logger_mutex);
	if (g_logger.joinable()) {
		g_logger.request_stop();
		g_logger.join();
	}
}

} // namespace Common::DebugCounters
