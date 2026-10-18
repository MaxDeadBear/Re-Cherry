// Calling recompiled guest functions from typed REX_HOOKs.
#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace cherry::core {

// The top of this thread's scratch guest stack for the next nested call, and its release; each nesting level gets its own region.
uint32_t AcquireGuestStack();
void ReleaseGuestStack();

// Runs a REX_IMPORT callable on a scratch guest stack. A typed hook is not told its caller's live r1, and the import's own frame would start from ThreadState's stale r1, on top of live frames.
template <typename Import, typename... Args>
auto CallGuest(const Import& import, Args... args) {
  rex::runtime::ThreadState* thread = rex::runtime::ThreadState::Get();
  const PPCContext* parent = thread->context();
  PPCContext ctx{};
  ctx.r1.u32 = AcquireGuestStack();
  ctx.r13 = parent->r13;
  ctx.fpscr = parent->fpscr;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  struct Release {
    ~Release() { ReleaseGuestStack(); }
  } release;
  return import(ctx, base, args...);
}

}  // namespace cherry::core
