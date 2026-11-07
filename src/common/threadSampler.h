#ifndef KYTY_COMMON_THREAD_SAMPLER_H_
#define KYTY_COMMON_THREAD_SAMPLER_H_

namespace Common::ThreadSampler {

// Statistical profiler for one host thread (Windows only). KYTY_SAMPLE_GPU="delay,duration"
// (seconds) samples the registering thread about once per millisecond after the delay, then
// writes thread-samples.txt with the hottest functions by exclusive and inclusive samples.
// guest: the thread runs guest code (no unwind data). Only frames inside the emulator image are
// unwound; ntdll's lookup for code outside any module crashed the sampler (gsamp-1).
void RegisterCurrentThreadFromEnvironment(const char* name, bool guest = false);

// Names the calling thread for host tools (Windows thread description; no-op elsewhere).
void SetCurrentThreadDescription(const char* name);

// Diagnostics: prints the calling thread's stack with symbols (Windows only).
void PrintCurrentStack(const char* label);
// The stack of a thread context (a Windows CONTEXT), e.g. from an exception record.
void PrintContextStack(const void* context, const char* label);

} // namespace Common::ThreadSampler

#endif /* KYTY_COMMON_THREAD_SAMPLER_H_ */
