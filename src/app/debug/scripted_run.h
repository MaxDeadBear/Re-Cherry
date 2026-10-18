#pragma once

#include <rex/rex_app.h>

namespace cherry::app::debug {

// With --cherry_debug_press, points the run at <user data>_scripted, refreshed from the player's saves and config at every start, so a scripted run's autosaves and menu presses never reach the player's own.
void IsolateScriptedRun(rex::PathConfig& paths);

}  // namespace cherry::app::debug
