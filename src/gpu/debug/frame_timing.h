#pragma once

#include <chrono>
#include <cstdint>

namespace cherry::gpu::debug {

enum class TimingPhase : uint32_t { Capture, Textures, Replay, FenceWait, Submit, Count };
enum class FrameCount : uint32_t { Pipelines, TextureBytes, GeometryBytes, Count };

bool FrameTimingEnabled();
void AddFrameTime(TimingPhase phase, std::chrono::steady_clock::duration elapsed);
// What the frame created for the first time, for the slow-frame log.
void AddFrameCount(FrameCount count, uint64_t amount);
// With --cherry_debug_frame_timing, logs every 150 presents the average milliseconds per frame each phase took and the rest of the frame (the guest).
void EndFrameTiming();
// On the guest thread at each submit: how long it waited for the render thread and slept for the frame rate cap; logs slow guest frames.
void NoteGuestFrame(std::chrono::steady_clock::duration waited, std::chrono::steady_clock::duration slept);

class ScopedFrameTimer {
 public:
  explicit ScopedFrameTimer(TimingPhase phase) : phase_(phase), enabled_(FrameTimingEnabled()) {
    if (enabled_) start_ = std::chrono::steady_clock::now();
  }
  ~ScopedFrameTimer() {
    if (enabled_) AddFrameTime(phase_, std::chrono::steady_clock::now() - start_);
  }

 private:
  TimingPhase phase_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace cherry::gpu::debug
