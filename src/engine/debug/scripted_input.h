#pragma once

#include <cstdint>

namespace cherry::engine::debug {

// True when --cherry_debug_press scripts pad 0; it then reads as connected even with no controller.
bool ScriptedPadActive();

// XINPUT_GAMEPAD buttons the script holds on pad 0 at the current presented frame.
uint16_t ScriptedButtons();

}  // namespace cherry::engine::debug
