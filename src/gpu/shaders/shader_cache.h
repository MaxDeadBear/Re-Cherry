// Declarations for res/shader_cache/shader_cache.cpp, which is generated with `#include "shader_cache.h"` and no header of its own.
#pragma once

#include <cstddef>
#include <cstdint>

// The initialiser order: { hash, dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask, textureDimensionMask, constantRegisterCount }, sorted by hash; tools/shaders/build_shader_cache.py writes it.
struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t spirvOffset;
  uint32_t spirvSize;
  uint32_t specConstantsMask;  // also kShaderIndexedVertexFetch
  // Two bits per fetch-constant slot below 16: 0 = 2D, 1 = 3D, 2 = cube.
  uint32_t textureDimensionMask;
  // One past the highest float4 constant register the shader reads (256 when it indexes them), so draws copy only that many.
  uint32_t constantRegisterCount = 256;
};

// SPEC_CONSTANT_INDEXED_VERTEX_FETCH in the patched shader_common.h: the vertex shader fetches at an index it computed, so its draw's vertices go to the vertex atlas.
inline constexpr uint32_t kShaderIndexedVertexFetch = 1u << 6;

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

// Each entry's DXIL, and its SPIR-V as smol-v, is one zstd frame at its offset and size, so a shader is decompressed only when the game creates it.
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
// The zstd dictionaries the frames were compressed with; empty when none was trained.
extern const uint8_t g_dxilCacheDictionary[];
extern const size_t g_dxilCacheDictionarySize;
extern const uint8_t g_spirvCacheDictionary[];
extern const size_t g_spirvCacheDictionarySize;
