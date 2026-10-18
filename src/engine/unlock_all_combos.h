#pragma once

#include <cstdint>

namespace cherry::engine {

// Applies or releases debug_unlock_all_combos; called once per game-thread engine tick.
void TickUnlockAllCombos();

// Called around every UnrealScript function body: puts the real flags back for the game's own writers and savers, follows a newly initialised ASH_PlayerPowerUpLV, and returns whether AfterScriptCallCombos must run.
bool BeforeScriptCallCombos(uint32_t object, uint32_t function);
void AfterScriptCallCombos();

}  // namespace cherry::engine
