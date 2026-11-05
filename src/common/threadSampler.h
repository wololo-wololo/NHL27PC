#ifndef KYTY_COMMON_THREAD_SAMPLER_H_
#define KYTY_COMMON_THREAD_SAMPLER_H_

namespace Common::ThreadSampler {

// Statistical profiler for one host thread (Windows only). KYTY_SAMPLE_GPU="delay,duration"
// (seconds) samples the registering thread about once per millisecond after the delay, then
// writes thread-samples.txt with the hottest functions by exclusive and inclusive samples.
void RegisterCurrentThreadFromEnvironment(const char* name);

// Diagnostics: prints the calling thread's stack with symbols (Windows only).
void PrintCurrentStack(const char* label);

} // namespace Common::ThreadSampler

#endif /* KYTY_COMMON_THREAD_SAMPLER_H_ */
