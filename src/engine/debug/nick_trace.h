#pragma once

#include <cstdint>

namespace cherry::engine::debug {

// Finds the script functions --cherry_debug_log_nick traces; called once per game-thread engine tick.
void TickNickTrace();

// Called around every UnrealScript function body: logs the Nick and Chainsaw Dash path with its arguments, result and gating state, and returns whether AfterScriptCallNickTrace must run.
bool BeforeScriptCallNickTrace(uint32_t object, uint32_t function, uint32_t locals);
void AfterScriptCallNickTrace(uint32_t result);

}  // namespace cherry::engine::debug
