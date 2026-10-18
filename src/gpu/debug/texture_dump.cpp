#include "gpu/debug/texture_dump.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include <rex/cvar.h>

#include "gpu/textures/texture_codec.h"
#include "gpu/textures/xenos_format.h"

REXCVAR_DEFINE_BOOL(cherry_debug_dump_textures, false, "Lollipop/Debug", "Write each block-compressed texture's first upload, decoded with alpha, to texture_dump/ as 32-bit TGA.");

namespace cherry::gpu::debug {

void DumpTexture(uint32_t address, uint32_t xenos_format, uint32_t width, uint32_t height, const uint8_t* blocks, uint32_t block_row_bytes) {
  if (!REXCVAR_GET(cherry_debug_dump_textures)) return;
  switch (xenos_format) {
    case kXenosFormat_DXT1:
    case kXenosFormat_DXT2_3:
    case kXenosFormat_DXT4_5:
    case kXenosFormat_DXT5A:
    case kXenosFormat_DXN: break;
    default: return;
  }
  static std::mutex mutex;
  static std::set<uint64_t> dumped;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!dumped.insert((uint64_t(address) << 32) | (width << 16) | height).second) return;
  }
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  DecodeBcToRgba(blocks, block_row_bytes, rgba.data(), width * 4, width, height, xenos_format);
  std::error_code ec;
  std::filesystem::create_directories("texture_dump", ec);
  std::ofstream out(std::filesystem::path("texture_dump") / std::format("{:08X}_{}x{}_f{}.tga", address, width, height, xenos_format), std::ios::binary);
  if (!out) return;
  // Uncompressed true-colour, 32 bits per pixel with 8 alpha bits, origin top-left.
  const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(width), uint8_t(width >> 8), uint8_t(height), uint8_t(height >> 8), 32, 0x28};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
  out.write(reinterpret_cast<const char*>(rgba.data()), std::streamsize(rgba.size()));
}

}  // namespace cherry::gpu::debug
