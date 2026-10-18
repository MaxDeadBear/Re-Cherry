// Linked instead of shader_cache.cpp until the cache has been generated (docs/renderer/shaders.md); every shader misses and is dumped.
#include "shader_cache.h"

ShaderCacheEntry g_shaderCacheEntries[1] = {};
const size_t g_shaderCacheEntryCount = 0;
const uint8_t g_compressedDxilCache[1] = {};
const size_t g_dxilCacheCompressedSize = 0;
const size_t g_dxilCacheDecompressedSize = 0;
const uint8_t g_compressedSpirvCache[1] = {};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
