#pragma once

#include <cstdint>

namespace cherry::gpu::debug {

// With --cherry_debug_dump_textures, writes a block-compressed texture's first upload, decoded with its alpha, to texture_dump/ADDRESS_WxH_fFORMAT.tga.
void DumpTexture(uint32_t address, uint32_t xenos_format, uint32_t width, uint32_t height, const uint8_t* blocks, uint32_t block_row_bytes);

}  // namespace cherry::gpu::debug
