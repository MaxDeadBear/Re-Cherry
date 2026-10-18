#include "gpu/hooks/hooks_internal.h"

#include <rex/hook.h>

#include "gpu/guest_memory.h"

// The functions the guest uses to wait for its GPU; with no guest GPU they would wait forever, so they are replaced rather than satisfied.

namespace {

using namespace cherry::gpu::hooks;

uint32_t D3D_CDevice_BlockOnFence_hook(uint32_t device, uint32_t fence, uint32_t reason, uint32_t flags) {
  (void)fence;
  (void)reason;
  (void)flags;
  PublishFences(device);
  return device;
}

uint32_t D3DDevice_BlockOnFence_hook(uint32_t fence) {
  (void)fence;
  const uint32_t device = GuestDevice();
  PublishFences(device);
  return device;
}

// The original also spins on dev+0x2B90, which only a GPU interrupt clears.
void D3DDevice_BlockUntilIdle_hook(uint32_t device) {
  PublishFences(device);
}

uint32_t D3DDevice_IsFencePending_hook(uint32_t fence) {
  (void)fence;
  PublishFences(GuestDevice());
  return 0;
}

// Pacing comes from the renderer's present and target_fps, not from waiting on vblank.
void D3DDevice_SynchronizeToPresentationInterval_hook(uint32_t device, uint32_t flip_address) {
  (void)device;
  (void)flip_address;
}

}  // namespace

// D3D_CDevice_BlockOnFence(D3DDevice*, DWORD fence, DWORD reason, BOOL no_kickoff)
REX_HOOK(D3D_CDevice_BlockOnFence, D3D_CDevice_BlockOnFence_hook)
// D3DDevice_BlockOnFence(DWORD fence)
REX_HOOK(D3DDevice_BlockOnFence, D3DDevice_BlockOnFence_hook)
// D3DDevice_BlockUntilIdle(D3DDevice*)
REX_HOOK(D3DDevice_BlockUntilIdle, D3DDevice_BlockUntilIdle_hook)
// D3DDevice_IsFencePending(DWORD fence) -> BOOL
REX_HOOK(D3DDevice_IsFencePending, D3DDevice_IsFencePending_hook)
// D3DDevice_SynchronizeToPresentationInterval(D3DDevice*, DWORD flip_address)
REX_HOOK(D3DDevice_SynchronizeToPresentationInterval, D3DDevice_SynchronizeToPresentationInterval_hook)
