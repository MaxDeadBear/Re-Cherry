#pragma once

#include <cstdint>
#include <vector>

namespace cherry::ui {

// Doubles a Flash movie's frame rate and puts a frame between each pair of its timeline frames in which every object that moved is halfway, so tweens step at 60 Hz instead of 30 while taking as long.
// A movie whose scripts could count frames (onEnterFrame, enterFrame clip events, numeric frame jumps, _currentframe) or that streams sound comes back empty, to be used unchanged.
std::vector<uint8_t> SmoothMovieTimeline(const std::vector<uint8_t>& movie);

}  // namespace cherry::ui
