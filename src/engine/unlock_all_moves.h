#pragma once

#include <cstdint>

namespace cherry::engine {

// Finds the script functions and properties debug_unlock_all_moves needs; called once per game-thread engine tick.
void TickUnlockAllMoves();

// Called around every UnrealScript function body: lends the story-gated state a move check needs, and returns whether AfterScriptCallMoves must run.
bool BeforeScriptCallMoves(uint32_t object, uint32_t function, uint32_t locals);
void AfterScriptCallMoves(uint32_t result);

}  // namespace cherry::engine
