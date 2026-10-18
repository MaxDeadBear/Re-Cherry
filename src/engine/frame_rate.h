#pragma once

#include <cstdint>

namespace cherry::engine {

// The frame rate the renderer's present holds the game to: 30 as shipped, 60, or 0 for no cap.
int TargetFps();

// Called once per presented frame by the D3DDevice_Swap hook.
void NotePresentedFrame();

// Frames presented so far.
uint64_t PresentedFrames();

// Seconds since the last presented frame, 0 before the first.
double SecondsSinceLastPresent();

// Average presented frames per second over the last measurement window, 0 until the first window closes.
double MeasuredFps();

}  // namespace cherry::engine
