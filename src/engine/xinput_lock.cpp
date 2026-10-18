#include <cstring>
#include <mutex>

#include "re_cherry_init.h"

#include <rex/hook.h>

#include "engine/debug/scripted_input.h"

// The SDK's InputSystem::RefreshDevices rebuilds its device list without a lock, and UE3 polls input from both its game and rendering threads, which crashed in that list's destructor; the guest's calls are serialised here.

namespace {

std::mutex g_input_mutex;

}  // namespace

// XInputGetCapabilities: a thunk to XamInputGetCapabilities(user_index, flags, XINPUT_CAPABILITIES*)
REX_HOOK_RAW(XInputGetCapabilities) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  const uint32_t user = ctx.r3.u32;
  const uint32_t caps = ctx.r5.u32;
  __imp__XInputGetCapabilities(ctx, base);
  // A scripted pad 0 reads as a wired gamepad (XINPUT_CAPABILITIES: Type, SubType, Flags, then zeroed gamepad and vibration).
  if (user == 0 && ctx.r3.u32 != 0 && caps && cherry::engine::debug::ScriptedPadActive()) {
    std::memset(base + caps, 0, 20);
    REX_STORE_U8(caps, 1);
    REX_STORE_U8(caps + 1, 1);
    ctx.r3.u64 = 0;
  }
}

// XInputGetState: a thunk to XamInputGetState(user_index, flags, XINPUT_STATE*)
REX_HOOK_RAW(XInputGetState) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r5.u32;
  __imp__XInputGetState(ctx, base);
  if (user != 0 || !state || !cherry::engine::debug::ScriptedPadActive()) return;
  // XINPUT_STATE: packet number, then XINPUT_GAMEPAD with its buttons first.
  if (ctx.r3.u32 != 0) {
    std::memset(base + state, 0, 16);
    ctx.r3.u64 = 0;
  }
  static uint32_t packet = 0;
  REX_STORE_U32(state, ++packet);
  REX_STORE_U16(state + 4, uint16_t(REX_LOAD_U16(state + 4) | cherry::engine::debug::ScriptedButtons()));
}

// XInputSetState: the wrapper that calls XamInputGetCapabilities, then XamInputSetState
REX_HOOK_RAW(XInputSetState) {
  std::lock_guard<std::mutex> lock(g_input_mutex);
  __imp__XInputSetState(ctx, base);
}
