#pragma once

#include <cstdint>

namespace cherry::engine {

// Re-asserts the cherry_gfx_shadow_resolution settings, which the game's own settings code may have reloaded from its ini; called every engine tick.
void ApplyShadowResolution(uint8_t* base);

}  // namespace cherry::engine
