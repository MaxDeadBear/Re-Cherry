#include "re_cherry_init.h"

#include <rex/hook.h>

#include "engine/debug/nick_trace.h"
#include "engine/debug/script_dump.h"
#include "engine/shadows.h"
#include "engine/unlock_all_combos.h"
#include "engine/unlock_all_moves.h"

// FEngineLoop::Tick(), once per frame on the game thread; GEngine->Tick (script included) has run when it returns.
REX_HOOK_RAW(sub_829BD280) {
  __imp__sub_829BD280(ctx, base);
  cherry::engine::debug::TickScriptDump();
  cherry::engine::debug::TickNickTrace();
  cherry::engine::TickUnlockAllCombos();
  cherry::engine::TickUnlockAllMoves();
  cherry::engine::ApplyShadowResolution(base);
}
