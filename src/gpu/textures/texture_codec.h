#pragma once

// Xenos texture bytes to host texture bytes, as pure functions over buffers with no device, cache or renderer state.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <plume_render_interface_types.h>

namespace cherry::gpu {

struct HostTextureFormat {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t block = 1;        // texels per block edge
  uint32_t block_bytes = 0;  // bytes per block (or per texel when block == 1)
  bool expand_to_rgba8 = false;
};

// The fetch constant's swizzle says where each output component comes from:
// four 3-bit selectors, 0..3 pick a source channel, 4 is constant 0, 5 is
// constant 1.
constexpr uint32_t kSwizzleIdentity = 0u | (1u << 3) | (2u << 6) | (3u << 9);

HostTextureFormat MapTextureFormat(uint32_t xenos_format);
plume::RenderFormat SnormTwin(plume::RenderFormat f);
// The components whose GPUTEXTURE_FETCH_CONSTANT.SignX..SignW (dword 0 bits 2..9) are GPUSIGN_GAMMA, bit 0 for X: the texture unit takes them from gamma to linear.
uint32_t GammaComponents(const uint32_t fetch_words[6]);
// The sRGB twin of a block-compressed host format, UNKNOWN when there is none.
plume::RenderFormat SrgbTwin(plume::RenderFormat f);
// RGBA8 texels to RGBA16 UNORM, the gamma components linearized; the sRGB curve stands in for the console's piecewise-linear 2.2 one.
std::vector<uint8_t> LinearizeGammaRgba8(const std::vector<uint8_t>& rgba8, uint32_t gamma_components);
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t width, uint32_t log2_bpp);
uint32_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height, uint32_t log2_bpp);
uint32_t Log2(uint32_t value);
void PackedTextureBaseOffset(uint32_t width, uint32_t height, uint32_t block_size, bool packed, uint32_t& x, uint32_t& y);
void ApplyEndian(uint8_t* data, size_t bytes, uint32_t endian);
// `src_bytes` is the size of one packed texel (2 or 4), not of the buffer.
void ExpandPackedToRgba8(const uint8_t* src, uint8_t* dst, uint32_t texels, uint32_t xenos_format, uint32_t src_bytes);
uint32_t FormatBytesPerBlock(plume::RenderFormat format);
uint32_t AlignRow(uint32_t bytes);
void ApplySwizzleToRgba8(const uint8_t* src, uint8_t* dst, size_t texels, uint32_t channels, uint32_t swizzle);
void DecodeBcToRgba(const uint8_t* src, uint32_t src_pitch, uint8_t* out_rgba, uint32_t out_pitch, uint32_t width, uint32_t height, uint32_t xenos_format);

}  // namespace cherry::gpu
