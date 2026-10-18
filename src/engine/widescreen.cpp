#include <bit>
#include <cmath>
#include <cstdint>

#include "core/hooks.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(cherry_gfx_ultrawide, 0, "Lollipop/Graphics", "Render wider than 16:9: 21 for 21:9, 32 for 32:9, 0 keeps the console's 16:9; takes effect at the next launch.").range(0, 32).validator([](std::string_view v) { return v == "0" || v == "21" || v == "32"; });

REXCVAR_DEFINE_INT32(cherry_gfx_fov, 100, "Lollipop/Graphics", "Camera field of view as a percentage of the game's: 90 to 130.").range(70, 150);

namespace {

constexpr double kConsoleAspect = 16.0 / 9.0;
// Set on the game thread while ULocalPlayer::CalcSceneView runs.
thread_local bool g_in_scene_view = false;

double UltrawideAspect() {
  switch (REXCVAR_GET(cherry_gfx_ultrawide)) {
    case 21: return 21.0 / 9.0;
    case 32: return 32.0 / 9.0;
    default: return 0.0;
  }
}

// A half-angle of view widened (or narrowed) by cherry_gfx_fov, through its tangent so the picture scales evenly.
double ScaledHalfFov(double half) {
  const int32_t percent = REXCVAR_GET(cherry_gfx_fov);
  return percent == 100 ? half : std::atan(std::tan(half) * double(percent) / 100.0);
}

}  // namespace

void CherryBackBufferWidth(PPCRegister& r11) {
  const double aspect = UltrawideAspect();
  if (aspect <= kConsoleAspect) return;
  r11.u64 = uint64_t(std::lround(aspect * 720.0));
  REXLOG_INFO("[display] back buffer {}x720 for {:.2f}:1", r11.u32, aspect);
}

bool CherryKeepsWideScreenBudget(PPCRegister& r11) {
  return r11.u32 > 1280;
}

// FPerspectiveMatrix::FPerspectiveMatrix_FOVXY(this, HalfFOVX, HalfFOVY, MultFOVX, MultFOVY), which ULocalPlayer::CalcSceneView builds the gameplay projection with: a landscape view passes MultFOVX 1 and MultFOVY width / height, keeping the horizontal FOV, so a view wider than 16:9 shows the same width and loses height.
// Past 16:9 the multipliers are rewritten to keep the console's vertical FOV and widen the horizontal one instead; cherry_gfx_fov scales both angles.
REX_HOOK_RAW(sub_82623FB0) {
  ctx.f1.f64 = ScaledHalfFov(ctx.f1.f64);
  ctx.f2.f64 = ScaledHalfFov(ctx.f2.f64);
  if (ctx.f3.f64 == 1.0 && ctx.f4.f64 > kConsoleAspect + 1e-3) {
    ctx.f3.f64 = kConsoleAspect / ctx.f4.f64;
    ctx.f4.f64 = kConsoleAspect;
  }
  __imp__sub_82623FB0(ctx, base);
}

// ULocalPlayer::CalcSceneView(this, ViewFamily, ViewLocation, ViewRotation, Viewport, ViewDrawer), the main view's projection; the perspective constructors below adjust only what it builds.
REX_HOOK_RAW(sub_8261FEE0) {
  g_in_scene_view = true;
  __imp__sub_8261FEE0(ctx, base);
  g_in_scene_view = false;
}

// FPerspectiveMatrix::FPerspectiveMatrix(this, HalfFOV, Width, Height, MinZ): a camera that constrains its aspect (bConstrainAspectRatio) builds its projection at that aspect, and the view rect would be pillarboxed to it, but the viewport reports a desired aspect of 16:9 whatever its size, so a wider back buffer is taken for 16:9 with wide pixels and the picture stretches.
// In the main view the horizontal term is narrowed by the camera's aspect over the screen's, so such a camera widens past 16:9 too; scene captures keep theirs.
REX_HOOK_RAW(sub_826240B8) {
  const uint32_t matrix = ctx.r3.u32;
  const double camera_aspect = ctx.f3.f64 != 0.0 ? ctx.f2.f64 / ctx.f3.f64 : 0.0;
  if (g_in_scene_view) ctx.f1.f64 = ScaledHalfFov(ctx.f1.f64);
  __imp__sub_826240B8(ctx, base);
  const double screen_aspect = UltrawideAspect();
  if (!g_in_scene_view || screen_aspect <= kConsoleAspect || camera_aspect <= 0.0 || camera_aspect >= screen_aspect) return;
  const float m00 = std::bit_cast<float>(REX_LOAD_U32(matrix));
  REX_STORE_U32(matrix, std::bit_cast<uint32_t>(float(m00 * camera_aspect / screen_aspect)));
}
