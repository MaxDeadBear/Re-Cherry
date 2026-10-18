#include "gpu/debug/bad_vertices.h"

#include <cmath>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/draw/draw_frame.h"
#include "gpu/draw/vertex_format.h"

REXCVAR_DEFINE_BOOL(cherry_debug_bad_vertices, false, "Lollipop/Debug", "Log draws whose decoded positions are NaN, infinite, denormal or beyond 1e7, with where their vertices were read from.");

namespace cherry::gpu::debug {

bool BadVertexCheckEnabled() {
  return REXCVAR_GET(cherry_debug_bad_vertices);
}

void CheckVertices(const HostLayout& layout, const uint8_t* vertices, uint32_t count, const std::function<std::string()>& describe) {
  if (!BadVertexCheckEnabled()) return;
  for (size_t e = 0; e < layout.elements.size(); ++e) {
    if (layout.elements[e].usage != kUsagePosition || layout.elements[e].usage_index != 0) continue;
    uint32_t bad = 0, first_bad = 0;
    float sample[4] = {};
    for (uint32_t v = 0; v < count; ++v) {
      float p[4];
      std::memcpy(p, vertices + size_t(v) * layout.stride() + e * 16, sizeof(p));
      for (int c = 0; c < 3; ++c) {
        const bool denormal = p[c] != 0.0f && std::fabs(p[c]) < 1e-30f;
        if (std::isnan(p[c]) || std::isinf(p[c]) || denormal || std::fabs(p[c]) > 1e7f) {
          if (!bad++) {
            first_bad = v;
            std::memcpy(sample, p, sizeof(sample));
          }
          break;
        }
      }
    }
    static uint32_t reported = 0;
    if (bad && reported++ < 40) REXGPU_WARN("[bad vertices] {} of {} vertices, first {} = ({}, {}, {}, {}); {}", bad, count, first_bad, sample[0], sample[1], sample[2], sample[3], describe());
  }
}

}  // namespace cherry::gpu::debug
