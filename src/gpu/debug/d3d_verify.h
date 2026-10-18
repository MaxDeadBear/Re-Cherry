#pragma once

#include <cstdint>

namespace cherry::gpu::debug {

// With --cherry_debug_verify_d3d, each of these re-runs the game's own function on the inputs a native hook just handled and logs any output that differs; the native outputs must already be in place.
void VerifyPadMipLevel(const uint32_t in[3], uint32_t width_io, uint32_t height_io, uint32_t depth_io, uint32_t bpp, uint32_t format, uint32_t dimension, uint32_t tiled, uint32_t native_size);
void VerifyCalcTextureLayout(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format, uint32_t dimension, uint32_t tiled, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t aligned_width_out, uint32_t base_size_out, uint32_t mip_size_out, uint32_t native_levels);
void VerifyGetMipLayout(uint32_t texture, uint32_t face, uint32_t level, uint32_t pitch_out, uint32_t slice_out, uint32_t size_out, uint32_t tail_out, uint32_t width_out, uint32_t height_out, uint32_t depth_out, uint32_t address_out);
void VerifySetTextureHeader(uint32_t type, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t texture, uint32_t base_size_out, uint32_t mip_size_out);

}  // namespace cherry::gpu::debug
