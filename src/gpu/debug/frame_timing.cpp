#include "gpu/debug/frame_timing.h"

#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(cherry_debug_frame_timing, false, "Lollipop/Debug", "Log every 150 presents how many milliseconds per frame went to capturing draws, uploading textures, recording the replay, waiting on the GPU, submitting, and the guest, and the slowest frame's split.");

namespace cherry::gpu::debug {

namespace {

constexpr uint32_t kFramesPerReport = 150;
constexpr uint32_t kPhases = uint32_t(TimingPhase::Count);

std::atomic<int64_t> g_totals[kPhases];
std::atomic<int64_t> g_frame_phases[kPhases];
std::atomic<uint64_t> g_frame_counts[uint32_t(FrameCount::Count)];
uint32_t g_slow_logged = 0;
std::atomic<int64_t> g_guest_capture{0};
std::chrono::steady_clock::time_point g_guest_last;
uint32_t g_guest_slow_logged = 0;
uint32_t g_guest_frames = 0;
int64_t g_worst_phases[kPhases];
int64_t g_worst_frame = 0;
uint32_t g_hitches = 0;
std::chrono::steady_clock::time_point g_window_start;
std::chrono::steady_clock::time_point g_frame_start;
uint32_t g_frames = 0;

double Ms(int64_t ns) { return double(ns) / 1e6; }

}  // namespace

bool FrameTimingEnabled() {
  return REXCVAR_GET(cherry_debug_frame_timing);
}

void AddFrameTime(TimingPhase phase, std::chrono::steady_clock::duration elapsed) {
  const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  g_totals[uint32_t(phase)] += ns;
  g_frame_phases[uint32_t(phase)] += ns;
  if (phase == TimingPhase::Capture) g_guest_capture += ns;
}

void NoteGuestFrame(std::chrono::steady_clock::duration waited, std::chrono::steady_clock::duration slept) {
  if (!FrameTimingEnabled()) return;
  const auto now = std::chrono::steady_clock::now();
  const int64_t capture = g_guest_capture.exchange(0);
  if (g_guest_last.time_since_epoch().count() == 0) {
    g_guest_last = now;
    return;
  }
  const int64_t cycle = std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_guest_last).count();
  g_guest_last = now;
  const int64_t wait = std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
  const int64_t sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(slept).count();
  if (++g_guest_frames % 150 == 0) g_guest_slow_logged = 0;
  if (cycle > 36'000'000 && g_guest_slow_logged++ < 10) REXGPU_INFO("[timing] guest frame {:.1f} ms: game {:.1f}, capture {:.1f}, waiting for the render thread {:.1f}, frame cap sleep {:.1f}", Ms(cycle), Ms(cycle - capture - wait - sleep), Ms(capture), Ms(wait), Ms(sleep));
}

void AddFrameCount(FrameCount count, uint64_t amount) {
  if (FrameTimingEnabled()) g_frame_counts[uint32_t(count)] += amount;
}

void EndFrameTiming() {
  if (!FrameTimingEnabled()) return;
  const auto now = std::chrono::steady_clock::now();
  if (g_frames++ == 0) {
    g_window_start = g_frame_start = now;
    for (auto& total : g_totals) total = 0;
    for (auto& phase : g_frame_phases) phase = 0;
    return;
  }
  // One frame: its wall time from the previous present, and the phases measured in it.
  const int64_t frame = std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_frame_start).count();
  g_frame_start = now;
  if (frame > 50'000'000) ++g_hitches;
  if (frame > 40'000'000 && g_slow_logged++ < 12) {
    REXGPU_INFO("[timing] slow frame {:.1f} ms: capture {:.2f}, textures {:.2f}, replay {:.2f}, submit {:.2f}; new pipelines {}, texture upload {} KB, geometry upload {} KB", Ms(frame), Ms(g_frame_phases[0]), Ms(g_frame_phases[1]), Ms(g_frame_phases[2]), Ms(g_frame_phases[4]), g_frame_counts[0].load(), g_frame_counts[1].load() >> 10, g_frame_counts[2].load() >> 10);
  }
  for (auto& count : g_frame_counts) count = 0;
  if (frame > g_worst_frame) {
    g_worst_frame = frame;
    for (uint32_t i = 0; i < kPhases; ++i) g_worst_phases[i] = g_frame_phases[i];
  }
  for (auto& phase : g_frame_phases) phase = 0;
  if (g_frames <= kFramesPerReport) return;
  int64_t t[kPhases];
  int64_t measured = 0;
  for (uint32_t i = 0; i < kPhases; ++i) measured += t[i] = g_totals[i].exchange(0);
  const int64_t window = std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_window_start).count();
  const double n = kFramesPerReport;
  REXGPU_INFO("[timing] {:.1f} ms/frame: capture {:.2f}, textures {:.2f}, replay {:.2f}, fence wait {:.2f}, submit {:.2f}, guest and pacing {:.2f}", Ms(window) / n, Ms(t[0]) / n, Ms(t[1]) / n, Ms(t[2]) / n, Ms(t[3]) / n, Ms(t[4]) / n, Ms(window - measured) / n);
  int64_t worst_measured = 0;
  for (int64_t phase : g_worst_phases) worst_measured += phase;
  REXGPU_INFO("[timing] slowest {:.1f} ms ({} over 50 ms): capture {:.2f}, textures {:.2f}, replay {:.2f}, fence wait {:.2f}, submit {:.2f}, guest {:.2f}", Ms(g_worst_frame), g_hitches, Ms(g_worst_phases[0]), Ms(g_worst_phases[1]), Ms(g_worst_phases[2]), Ms(g_worst_phases[3]), Ms(g_worst_phases[4]), Ms(g_worst_frame - worst_measured));
  g_worst_frame = 0;
  g_hitches = 0;
  g_slow_logged = 0;
  g_window_start = now;
  g_frames = 1;
}

}  // namespace cherry::gpu::debug
