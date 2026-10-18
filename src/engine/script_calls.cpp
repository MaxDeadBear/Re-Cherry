#include "re_cherry_init.h"

#include <rex/hook.h>

#include "engine/debug/nick_trace.h"
#include "engine/unlock_all_combos.h"
#include "engine/unlock_all_moves.h"

// UObject::ProcessInternal(FFrame& Stack, void* Result), which runs every UnrealScript function body on this (r3); Stack.Node (+0x10) is the UFunction and Stack.Locals (+0x1C) its parameters and locals.
REX_HOOK_RAW(sub_8227DA48) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t function = REX_LOAD_U32(ctx.r4.u32 + 0x10);
  const uint32_t locals = REX_LOAD_U32(ctx.r4.u32 + 0x1C);
  const uint32_t result = ctx.r5.u32;
  const bool trace = cherry::engine::debug::BeforeScriptCallNickTrace(object, function, locals);
  const bool combos = cherry::engine::BeforeScriptCallCombos(object, function);
  const bool moves = cherry::engine::BeforeScriptCallMoves(object, function, locals);
  __imp__sub_8227DA48(ctx, base);
  if (moves) cherry::engine::AfterScriptCallMoves(result);
  if (combos) cherry::engine::AfterScriptCallCombos();
  if (trace) cherry::engine::debug::AfterScriptCallNickTrace(result);
}
