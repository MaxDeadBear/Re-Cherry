// Xenos texture data formats (the fetch constant's dword 1, bits 0..5).
#pragma once

#include <cstdint>

namespace cherry::gpu {

enum GuestTextureFormat : uint32_t {
  kXenosFormat_1_REVERSE = 0,
  kXenosFormat_1 = 1,
  kXenosFormat_8 = 2,
  kXenosFormat_1_5_5_5 = 3,
  kXenosFormat_5_6_5 = 4,
  kXenosFormat_6_5_5 = 5,
  kXenosFormat_8_8_8_8 = 6,
  kXenosFormat_2_10_10_10 = 7,
  kXenosFormat_8_A = 8,
  kXenosFormat_8_B = 9,
  kXenosFormat_8_8 = 10,
  kXenosFormat_4_4_4_4 = 15,
  kXenosFormat_10_11_11 = 16,
  kXenosFormat_11_11_10 = 17,
  kXenosFormat_DXT1 = 18,
  kXenosFormat_DXT2_3 = 19,
  kXenosFormat_DXT4_5 = 20,
  kXenosFormat_24_8 = 22,
  kXenosFormat_24_8_FLOAT = 23,
  kXenosFormat_16 = 24,
  kXenosFormat_16_16 = 25,
  kXenosFormat_16_16_16_16 = 26,
  kXenosFormat_16_FLOAT = 30,
  kXenosFormat_16_16_FLOAT = 31,
  kXenosFormat_16_16_16_16_FLOAT = 32,
  kXenosFormat_32_FLOAT = 36,
  kXenosFormat_32_32_FLOAT = 37,
  kXenosFormat_32_32_32_32_FLOAT = 38,
  kXenosFormat_DXN = 49,
  kXenosFormat_8_8_8_8_AS_16_16_16_16 = 50,
  kXenosFormat_DXT1_AS_16_16_16_16 = 51,
  kXenosFormat_DXT2_3_AS_16_16_16_16 = 52,
  kXenosFormat_DXT4_5_AS_16_16_16_16 = 53,
  kXenosFormat_2_10_10_10_AS_16_16_16_16 = 54,
  kXenosFormat_DXT3A = 58,
  kXenosFormat_DXT5A = 59,
  kXenosFormat_CTX1 = 60,
};

}  // namespace cherry::gpu
