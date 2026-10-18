#pragma once

namespace cherry::app::debug {

// With --cherry_debug_profile_frame=N, samples every thread's instruction pointer from presented frame N for --cherry_debug_profile_seconds and logs each busy thread's hottest functions, self and inclusive.
void StartSamplingProfiler();
void StopSamplingProfiler();

}  // namespace cherry::app::debug
