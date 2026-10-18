#include "engine/frame_rate.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "re_cherry_init.h"

REXCVAR_DEFINE_INT32(target_fps, 30, "Lollipop/Performance", "Target frame rate cap. 30 = original, 60 = unlocked, 0 = uncapped (the display's refresh rate with vsync)")
    .range(0, 60)
    .validator([](std::string_view v) { return v == "0" || v == "30" || v == "60"; });

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFramesPerWindow = 60;

std::atomic<double> g_measured_fps{0.0};
std::atomic<int64_t> g_last_present_ns{0};
std::atomic<uint64_t> g_presented_frames{0};
Clock::time_point g_window_start;
int g_window_frames = 0;

}  // namespace

namespace cherry::engine {

int TargetFps() {
  const int32_t fps = REXCVAR_GET(target_fps);
  return fps == 0 ? 0 : fps == 60 ? 60 : 30;
}

void NotePresentedFrame() {
  const auto now = Clock::now();
  g_last_present_ns.store(now.time_since_epoch().count(), std::memory_order_relaxed);
  g_presented_frames.fetch_add(1, std::memory_order_relaxed);
  if (g_window_frames == 0) {
    g_window_start = now;
  } else if (g_window_frames == kFramesPerWindow) {
    const std::chrono::duration<double> elapsed = now - g_window_start;
    g_measured_fps.store(kFramesPerWindow / elapsed.count(), std::memory_order_relaxed);
    g_window_start = now;
    g_window_frames = 0;
  }
  ++g_window_frames;
}

uint64_t PresentedFrames() {
  return g_presented_frames.load(std::memory_order_relaxed);
}

double SecondsSinceLastPresent() {
  const int64_t last = g_last_present_ns.load(std::memory_order_relaxed);
  if (last == 0) return 0.0;
  return std::chrono::duration<double>(Clock::now().time_since_epoch() - Clock::duration(last)).count();
}

double MeasuredFps() {
  return g_measured_fps.load(std::memory_order_relaxed);
}

}  // namespace cherry::engine

// UGameEngine::GetMaxTickRate(this, DeltaTime) -> float: the engine's MaxTickRate (this+1208), which appUpdateTimeAndHandleMaxTickRate sleeps the game thread to in whole milliseconds, landing near 58 when it is 60; 0 leaves pacing to the present's target_fps.
REX_HOOK_RAW(sub_8253A7C0) {
  ctx.f1.f64 = 0.0;
}
