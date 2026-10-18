// Texture memory layout as the game's own D3D and XG code computes it (XGPadMipLevel, XGGetMipTailLevelOffset, D3DBaseTexture_GetMipLayout).
#pragma once

#include <cstdint>

namespace cherry::gpu {

// Bits per texel of a texture format, from the game's table at 0x821A54F0.
uint32_t FormatBitsPerTexel(uint32_t format);
// The low four bits of the same table's flag byte.
uint32_t FormatFlags(uint32_t format);
void FormatBlockSize(uint32_t format, uint32_t& block_width, uint32_t& block_height);

// Pads a level's size to its tile or pitch alignment and returns its size in bytes (XGPadMipLevel).
uint32_t PadMipLevel(uint32_t& width, uint32_t& height, uint32_t& depth, uint32_t bpp, uint32_t format, uint32_t dimension, uint32_t tiled);
// Where a level sits inside the packed mip tail: block offsets and the byte offset (XGGetMipTailLevelOffset).
uint32_t MipTailOffset(uint32_t level, uint32_t width, uint32_t height, uint32_t depth, uint32_t row_pitch, uint32_t slice_pitch, uint32_t format, uint32_t& x, uint32_t& y, uint32_t& z);

// One level of the texture a GPUTEXTURE_FETCH_CONSTANT describes, as D3DBaseTexture_GetMipLayout lays it out.
struct MipLocation {
  uint32_t address = 0;      // the level's start, or the tail's for a level inside the packed tail
  uint32_t row_pitch = 0;    // bytes per row of texels
  uint32_t rows = 0;         // padded rows of texels
  uint32_t width = 0;        // the level's own size
  uint32_t height = 0;
  uint32_t tail_x = 0;       // offset in blocks inside the tail
  uint32_t tail_y = 0;
  bool in_tail = false;
};
// A cube face or stack slice (`layer`) sits one page-aligned slice after the previous; volumes keep their slices together.
MipLocation LocateMip(const uint32_t fetch[6], uint32_t level, uint32_t layer = 0);

}  // namespace cherry::gpu
