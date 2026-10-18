#include "core/guest_call.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace cherry::core {

namespace {

constexpr uint32_t kLevelBytes = 64 * 1024;
constexpr uint32_t kLevels = 4;
// Space left above each region's top for the callee's caller-side save area.
constexpr uint32_t kTopReserve = 0x100;

thread_local uint32_t t_stack = 0;
thread_local uint32_t t_depth = 0;

}  // namespace

uint32_t AcquireGuestStack() {
  if (!t_stack) {
    rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeapByType(false, 4096);
    if (!heap || !heap->Alloc(kLevelBytes * kLevels, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &t_stack)) {
      REXSYS_ERROR("[core] no guest memory for a scratch call stack");
      t_stack = 0;
    }
  }
  if (t_depth >= kLevels) REXSYS_ERROR("[core] guest calls nested {} deep; the scratch stack is reused", t_depth + 1);
  const uint32_t level = t_depth < kLevels ? t_depth : kLevels - 1;
  ++t_depth;
  return t_stack + kLevelBytes * (kLevels - level) - kTopReserve;
}

void ReleaseGuestStack() {
  if (t_depth) --t_depth;
}

}  // namespace cherry::core
