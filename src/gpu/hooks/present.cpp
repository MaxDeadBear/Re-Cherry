#include "gpu/hooks/hooks_internal.h"

#include <rex/hook.h>

#include "engine/frame_rate.h"
#include "gpu/draw/draw_frame.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"

namespace {

using namespace cherry::gpu;
using namespace cherry::gpu::hooks;

// UE3's present resolves the back buffer into pFrontBuffer and swaps it; the native present shows that resolve's host texture.
void D3DDevice_Swap_hook(uint32_t device, uint32_t front_buffer, uint32_t scaler_parameters) {
  (void)scaler_parameters;
  // The swapped texture's fetch constant, six dwords from +0x1C: the address the frame's last resolve wrote.
  TextureFetch front;
  for (uint32_t w = 0; front_buffer && w < 6; ++w) front.words[w] = LoadGuestU32(front_buffer + 0x1C + 4 * w);
  PublishFences(device);
  StoreGuestU32(device + dev::kFrameCounter, LoadGuestU32(device + dev::kFrameCounter) + 1);
  // Taken before the data sink resets: Begin*Vertices draws still read their vertices from it.
  static DrawFrame frame;
  TakeCapturedFrame(frame);
  if (RingSinksReady()) {
    RewindCommandRing(device);
    ResetDataSink();
  }
  if (GraphicsSystem* graphics = GraphicsSystem::instance()) graphics->SubmitFrame(frame, front);
  cherry::engine::NotePresentedFrame();
  static uint64_t swaps = 0;
  if ((swaps++ % 600) == 0) LogRingStats(device);
}

}  // namespace

// D3DDevice_Swap(D3DDevice*, D3DBaseTexture* pFrontBuffer, const D3DVIDEO_SCALER_PARAMETERS*)
REX_HOOK(D3DDevice_Swap, D3DDevice_Swap_hook)
