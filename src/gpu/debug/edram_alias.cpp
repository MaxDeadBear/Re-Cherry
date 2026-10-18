#include "gpu/debug/edram_alias.h"

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/draw/draw_frame.h"

REXCVAR_DEFINE_BOOL(cherry_debug_edram_alias, false, "Lollipop/Debug", "Log, every 150 frames, draws and resolves that read EDRAM tiles a different surface (format, size or base) last wrote.");

namespace cherry::gpu::debug {

namespace {

constexpr uint32_t kTiles = 2048;
constexpr uint32_t kFramesPerReport = 150;

// A host render target's identity: EDRAM base and format, size, depth.
using SurfaceId = std::tuple<uint32_t, uint32_t, uint32_t, bool>;

SurfaceId Id(const SurfaceBinding& s, bool depth) {
  return {SurfaceKey(s.info, depth), s.width, s.height, depth};
}

std::vector<SurfaceId> g_owner(kTiles);
std::map<std::tuple<const char*, SurfaceId, SurfaceId>, uint32_t> g_events;
uint32_t g_frames = 0;

// GPUEDRAMCOLORFORMAT 16_16_16_16, 16_16_16_16_FLOAT, 2_10_10_10_FLOAT_AS_16_16_16_16 and 32_32_FLOAT take two 32-bit samples per pixel.
bool Wide(const SurfaceBinding& s, bool depth) {
  if (depth) return false;
  const uint32_t format = (s.info >> 16) & 0xF;
  return format == 5 || format == 7 || format == 12 || format == 15;
}

// The EDRAM tiles (80x16 32-bit samples) a rectangle of the surface covers; GPU_SURFACEINFO gives the pitch in pixels and MsaaSamples (2x doubles the rows, 4x also the columns).
template <typename F>
void ForTiles(const SurfaceBinding& s, bool depth, int32_t x0, int32_t y0, int32_t x1, int32_t y1, F&& visit) {
  const uint32_t msaa = (s.surface_info >> 16) & 3;
  const uint32_t sx = (msaa == 2 ? 2 : 1) * (Wide(s, depth) ? 2 : 1);
  const uint32_t sy = msaa ? 2 : 1;
  const uint32_t pitch = s.surface_info & 0x3FFF ? s.surface_info & 0x3FFF : s.width;
  const uint32_t columns = (pitch * sx + 79) / 80;
  x0 = std::clamp(x0, 0, int32_t(s.width));
  x1 = std::clamp(x1, x0, int32_t(s.width));
  y0 = std::clamp(y0, 0, int32_t(s.height));
  y1 = std::clamp(y1, y0, int32_t(s.height));
  const uint32_t base = s.info & 0xFFF;
  for (uint32_t ty = uint32_t(y0) * sy / 16; ty < (uint32_t(y1) * sy + 15) / 16; ++ty) {
    for (uint32_t tx = uint32_t(x0) * sx / 80; tx < (uint32_t(x1) * sx + 79) / 80; ++tx) visit((base + ty * columns + tx) % kTiles);
  }
}

void Read(const char* kind, const SurfaceBinding& s, bool depth, const int32_t rect[4]) {
  if (!s.valid()) return;
  const SurfaceId id = Id(s, depth);
  SurfaceId other;
  bool foreign = false;
  ForTiles(s, depth, rect[0], rect[1], rect[2], rect[3], [&](uint32_t tile) {
    if (!foreign && std::get<1>(g_owner[tile]) && g_owner[tile] != id) {
      foreign = true;
      other = g_owner[tile];
    }
  });
  if (foreign) {
    ++g_events[{kind, id, other}];
    static uint32_t detailed = 0;
    if (detailed++ < 8) REXGPU_INFO("[edram] {} of {}x{} info {:08X} surface info {:08X}", kind, s.width, s.height, s.info, s.surface_info);
  }
}

void Write(const SurfaceBinding& s, bool depth, const int32_t rect[4]) {
  if (!s.valid()) return;
  const SurfaceId id = Id(s, depth);
  ForTiles(s, depth, rect[0], rect[1], rect[2], rect[3], [&](uint32_t tile) { g_owner[tile] = id; });
}

bool BlendReadsTarget(uint32_t blend) {
  auto reads = [](uint32_t factor) { return factor >= 8 && factor <= 11; };
  return (blend >> 8 & 0x1F) || (blend >> 24 & 0x1F) || reads(blend & 0x1F) || reads(blend >> 16 & 0x1F);
}

std::string Describe(const SurfaceId& id) {
  return std::format("{}x{} {} base {} format {}", std::get<1>(id), std::get<2>(id), std::get<3>(id) ? "depth" : "colour", std::get<0>(id) & 0xFFF, (std::get<0>(id) >> 16) & 0xF);
}

}  // namespace

void TrackEdram(const DrawFrame& frame) {
  if (!REXCVAR_GET(cherry_debug_edram_alias)) return;
  for (const DrawRecord& r : frame.draws) {
    const int32_t full_color[4] = {0, 0, int32_t(r.color.width), int32_t(r.color.height)};
    const int32_t full_depth[4] = {0, 0, int32_t(r.depth.width), int32_t(r.depth.height)};
    if (r.kind == CommandKind::Clear) {
      const int32_t* color_rect = r.clear_has_rect ? r.clear_rect : full_color;
      const int32_t* depth_rect = r.clear_has_rect ? r.clear_rect : full_depth;
      if (r.clear_flags & 1) Write(r.color, false, color_rect);
      for (uint32_t i = 0; i < 3; ++i) {
        if (r.clear_flags & (2u << i)) Write(r.extra_colors[i], false, color_rect);
      }
      if (r.clear_flags & 0x30) Write(r.depth, true, depth_rect);
    } else if (r.kind == CommandKind::Resolve) {
      const bool depth = (r.resolve_flags & 7) == 4;
      Read(depth ? "resolve depth" : "resolve colour", r.resolve_source, depth, r.resolve_rect);
      if (r.resolve_flags & 0x100) Write(r.resolve_source, false, r.resolve_rect);
      if (r.resolve_flags & 0x200) Write(r.depth, true, full_depth);
    } else {
      // DepthControl: stencil bit 0, z test bit 1, z write bit 2.
      if (r.depth_control & 3) Read("depth test", r.depth, true, r.scissor);
      if ((r.color_mask & 0xF) && BlendReadsTarget(r.blend_control)) Read("blend", r.color, false, r.scissor);
      if (r.color_mask & 0xF) Write(r.color, false, r.scissor);
      for (uint32_t i = 0; i < 3; ++i) {
        if ((r.color_mask >> (4 * (i + 1))) & 0xF) Write(r.extra_colors[i], false, r.scissor);
      }
      if (r.depth_control & 5) Write(r.depth, true, r.scissor);
    }
  }
  if (++g_frames < kFramesPerReport) return;
  g_frames = 0;
  std::vector<std::pair<uint32_t, std::tuple<const char*, SurfaceId, SurfaceId>>> sorted;
  for (const auto& [key, count] : g_events) sorted.emplace_back(count, key);
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  REXGPU_INFO("[edram] {} kinds of read from tiles another surface wrote, over {} frames", sorted.size(), kFramesPerReport);
  for (size_t i = 0; i < std::min<size_t>(sorted.size(), 16); ++i) {
    const auto& [count, key] = sorted[i];
    REXGPU_INFO("[edram]   {} x{}: {} over tiles from {}", std::get<0>(key), count, Describe(std::get<1>(key)), Describe(std::get<2>(key)));
  }
  g_events.clear();
}

}  // namespace cherry::gpu::debug
