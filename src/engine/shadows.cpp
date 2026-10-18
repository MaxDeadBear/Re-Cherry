#include "engine/shadows.h"

#include <algorithm>
#include <bit>
#include <cstdint>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(cherry_gfx_shadow_resolution, 0, "Lollipop/Graphics", "Character shadow map size: 0 keeps the game's 128, 256, 512 or 1024 draw them that much sharper; takes effect at the next launch.").range(0, 1024).validator([](std::string_view v) { return v == "0" || v == "128" || v == "256" || v == "512" || v == "1024"; });

namespace {

// GSystemSettings (0x833692D0) as Xbox360-AshEngine.ini sets it: MaxShadowResolution=128, ShadowTexelsPerPixel=1.27324. The render thread reads its own copy of the resolutions, filled from these when settings are activated.
constexpr uint32_t kMaxShadowResolution = 0x833696A8;
constexpr uint32_t kShadowTexelsPerPixel = 0x833696B0;
constexpr uint32_t kRenderMaxShadowResolution = 0x83369740;
// FSceneRenderTargets sizes the shadow depth atlas at most 1120x688, and a per-object shadow keeps a 10-texel border inside it.
constexpr int32_t kMaxResolution = 1024;

int32_t g_resolution = 0;      // the shadow resolution this run uses, fixed at startup since the atlas is sized once
float g_texels_per_pixel = 0;  // the game's own ShadowTexelsPerPixel, scaled by the same factor

}  // namespace

namespace cherry::engine {

void ApplyShadowResolution(uint8_t* base) {
  if (!g_resolution) return;
  if (int32_t(REX_LOAD_U32(kMaxShadowResolution)) != g_resolution) REX_STORE_U32(kMaxShadowResolution, uint32_t(g_resolution));
  if (int32_t(REX_LOAD_U32(kRenderMaxShadowResolution)) != g_resolution) REX_STORE_U32(kRenderMaxShadowResolution, uint32_t(g_resolution));
  REX_STORE_U32(kShadowTexelsPerPixel, std::bit_cast<uint32_t>(g_texels_per_pixel));
}

}  // namespace cherry::engine

// FSystemSettings::Initialize(this, bSetupForEditor): loads [SystemSettings] and copies it for the render thread, before the scene render targets are sized from it.
// A per-object shadow is projected radius x ShadowTexelsPerPixel texels, clamped to MaxShadowResolution and rounded down to a power of two below it, so both grow together or mid-distance shadows round back down.
REX_HOOK_RAW(sub_823CBD58) {
  __imp__sub_823CBD58(ctx, base);
  const int32_t wanted = std::min(REXCVAR_GET(cherry_gfx_shadow_resolution), kMaxResolution);
  const int32_t game = int32_t(REX_LOAD_U32(kMaxShadowResolution));
  if (wanted <= game || game <= 0) return;
  g_resolution = wanted;
  g_texels_per_pixel = std::bit_cast<float>(REX_LOAD_U32(kShadowTexelsPerPixel)) * float(wanted) / float(game);
  cherry::engine::ApplyShadowResolution(base);
  REXLOG_INFO("[shadows] character shadows up to {} texels (the game's {}), {:.2f} texels per pixel", wanted, game, g_texels_per_pixel);
}
