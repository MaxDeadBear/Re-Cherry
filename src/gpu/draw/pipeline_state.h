// Everything a draw pipeline is built from, as plain data: the key pipelines are cached by, and the record pipelines.bin keeps for building them ahead at the next launch.
#pragma once

#include <cstdint>

namespace cherry::gpu {

struct PipelineElement {
  uint8_t usage = 0;
  uint8_t usage_index = 0;
  uint8_t location = 0;
  uint8_t integer = 0;
};

// Fields the pipeline does not depend on are zeroed, so draws differing only there share one.
struct PipelineState {
  static constexpr uint32_t kMaxElements = 32;
  uint64_t vs_hash = 0;
  uint64_t ps_hash = 0;  // 0 for a depth-only draw
  uint32_t topology = 0;  // DrawTopology
  uint32_t blend_control = 0;
  uint32_t extra_blend_controls[3] = {};
  uint32_t depth_control = 0;
  uint32_t stencil_ref_mask = 0;
  uint32_t mode_control = 0;
  uint32_t color_mask = 0;
  uint32_t color_formats = 0;  // plume::RenderFormat per target, a byte each
  uint32_t depth_format = 0;
  float slope_scaled_depth_bias = 0.0f;
  uint32_t element_count = 0;
  PipelineElement elements[kMaxElements];
};

}  // namespace cherry::gpu
