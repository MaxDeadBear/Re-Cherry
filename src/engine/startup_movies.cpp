#include <cstdint>
#include <string>
#include <string_view>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(skip_startup_logos, true, "Lollipop/Patches", "Skip the publisher and middleware logo movies at startup (Warner, Kadokawa, Grasshopper, Unreal, Scaleform).");

namespace {

// The boot sequence's logo movies, in the order it plays them.
constexpr std::string_view kLogoMovies[] = {"CL_Warner", "CL_Kadokawa", "CL_GhM", "CL_UTech", "CL_Scaleform"};

std::string MovieName(uint8_t* base, uint32_t name) {
  std::string out;
  for (uint32_t i = 0; name && i < 64; ++i) {
    const uint16_t c = REX_LOAD_U16(name + i * 2);
    if (!c) break;
    out.push_back(c < 128 ? char(c) : '?');
  }
  return out;
}

}  // namespace

// FFullScreenMovieGears::GameThreadPlayMovie(this, Flags, MovieName, ...): a logo is started as usual, so the boot sequence's checks on it by name see it, and stopped at once, as skipping it would.
REX_HOOK_RAW(sub_82331780) {
  const uint32_t player = ctx.r3.u32;
  bool logo = false;
  if (REXCVAR_GET(skip_startup_logos)) {
    const std::string name = MovieName(base, ctx.r5.u32);
    for (std::string_view movie : kLogoMovies) logo = logo || name == movie;
  }
  __imp__sub_82331780(ctx, base);
  if (!logo) return;
  // FFullScreenMovieGears::GameThreadStopMovie(this, DelayInSeconds, bWaitForMovie, bForceStop, bStopRendering)
  const uint64_t result = ctx.r3.u64;
  ctx.r3.u64 = player;
  ctx.f1.f64 = 0.0;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 1;
  __imp__sub_82331EC8(ctx, base);
  ctx.r3.u64 = result;
}
