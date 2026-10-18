#include "gpu/shaders/shader_registry.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <smolv.h>
#include <zstd.h>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/graphics_system.h"
#include "gpu/shaders/shader_cache.h"

REXCVAR_DEFINE_BOOL(cherry_gfx_dump_shaders, false, "Lollipop/Graphics", "Dump every shader container the game creates to shader_dump/, not only the ones missing from the cache; a full cache rebuild needs this.");

namespace cherry::gpu {

namespace {

constexpr uint32_t kContainerMagicMask = 0xFFFFFF00;
constexpr uint32_t kContainerMagic = 0x102A1100;
constexpr uint32_t kMaxContainerBytes = 1u << 20;

std::mutex g_mutex;
std::filesystem::path g_dump_directory;
// Shader objects are freed and their addresses reused, so only the newest binding is right.
std::unordered_map<uint32_t, const ShaderCacheEntry*> g_bindings;
std::unordered_map<uint32_t, VertexShaderBinding> g_vertex_bindings;
std::unordered_set<uint64_t> g_dumped;
uint32_t g_hits = 0;
uint32_t g_misses = 0;
uint32_t g_rejected = 0;

std::once_flag g_blobs_once;
std::vector<uint8_t> g_dxil;
std::vector<uint8_t> g_spirv_smolv;
std::unordered_map<const ShaderCacheEntry*, std::vector<uint8_t>> g_spirv;

const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  const ShaderCacheEntry* begin = g_shaderCacheEntries;
  const ShaderCacheEntry* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  const ShaderCacheEntry* found = std::lower_bound(begin, end, hash, [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return found != end && found->hash == hash ? found : nullptr;
}

void DumpContainer(const uint8_t* data, size_t size, uint64_t hash) {
  if (g_dump_directory.empty() || !g_dumped.insert(hash).second) return;
  std::error_code ec;
  std::filesystem::create_directories(g_dump_directory, ec);
  std::ofstream(g_dump_directory / std::format("{:016X}.bin", hash), std::ios::binary).write(reinterpret_cast<const char*>(data), std::streamsize(size));
}

std::vector<uint8_t> Decompress(const uint8_t* data, size_t compressed, size_t decompressed) {
  std::vector<uint8_t> out(decompressed);
  if (compressed == 0) return out;
  if (compressed == decompressed) {
    std::copy_n(data, decompressed, out.data());
    return out;
  }
  const size_t written = ZSTD_decompress(out.data(), out.size(), data, compressed);
  if (ZSTD_isError(written) || written != decompressed) {
    REXGPU_ERROR("[gfx] shader cache blob failed to decompress ({})", ZSTD_isError(written) ? ZSTD_getErrorName(written) : "size mismatch");
    out.clear();
  }
  return out;
}

void LoadBlobs() {
  std::call_once(g_blobs_once, [] {
    g_dxil = Decompress(g_compressedDxilCache, g_dxilCacheCompressedSize, g_dxilCacheDecompressedSize);
    g_spirv_smolv = Decompress(g_compressedSpirvCache, g_spirvCacheCompressedSize, g_spirvCacheDecompressedSize);
  });
}

}  // namespace

void SetShaderDumpDirectory(const std::filesystem::path& directory) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dump_directory = directory;
  REXGPU_INFO("[gfx] shader cache has {} entries; containers dump to {}", g_shaderCacheEntryCount, directory.string());
}

// The hash is over the container bytes as the game loaded them, virtual part then physical part, big-endian header included, exactly as XenosRecomp hashes the dumped files.
void RegisterShader(uint8_t* base, uint32_t virtual_part, uint32_t physical_part, uint32_t object, bool pixel_shader) {
  if (!virtual_part || !physical_part || !object) return;
  const uint32_t container = virtual_part;
  const uint32_t flags = REX_LOAD_U32(container);
  const uint32_t virtual_size = REX_LOAD_U32(container + 4);
  const uint32_t total = virtual_size + REX_LOAD_U32(container + 8);
  if ((flags & kContainerMagicMask) != kContainerMagic || total == 0 || total > kMaxContainerBytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_rejected++ < 8) REXGPU_WARN("[gfx] {} container at {:08X} is not a 0x102A11xx container (flags {:08X}, {} bytes)", pixel_shader ? "pixel" : "vertex", container, flags, total);
    return;
  }
  std::vector<uint8_t> bytes(total);
  std::memcpy(bytes.data(), base + virtual_part, virtual_size);
  std::memcpy(bytes.data() + virtual_size, base + physical_part, total - virtual_size);
  const uint8_t* data = bytes.data();
  const uint64_t hash = XXH3_64bits(data, total);
  const ShaderCacheEntry* entry = FindShaderCacheEntry(hash);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bindings.insert_or_assign(object, entry);
    if (!pixel_shader) g_vertex_bindings.erase(object);
    if (entry) ++g_hits;
    else ++g_misses;
    if (!entry || REXCVAR_GET(cherry_gfx_dump_shaders)) DumpContainer(data, total, hash);
    const uint32_t created = g_hits + g_misses;
    if (created <= 4 || (created % 500) == 0) REXGPU_INFO("[gfx] {} shader {:016X} ({} bytes, flags {:08X}) {}; {} created, {} translated, {} missing", pixel_shader ? "pixel" : "vertex", hash, total, flags, entry ? "translated" : "missing", created, g_hits, g_misses);
  }
  // Built now, outside the lock, so a blob that fails to load is reported when its shader is created rather than at its first draw.
  if (GraphicsSystem* graphics = GraphicsSystem::instance(); graphics && entry) graphics->ShaderModule(entry);
}

// XeVertexShader_Create copies the container's virtual part to object + 872 and XGRegisterVertexShader stores the microcode's address at + 0x20.
// In the virtual part, shaderOffset (+0x18) locates the Shader header, whose physicalOffset finds the code in the physical part; the element table follows the header.
void NoteVertexShaderBinding(uint32_t object, const VertexShaderBinding& binding) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_vertex_bindings[object] = binding;
}

bool LookupVertexShaderBinding(uint32_t object, VertexShaderBinding* binding) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_vertex_bindings.find(object);
  if (found == g_vertex_bindings.end()) return false;
  *binding = found->second;
  return true;
}

const ShaderCacheEntry* LookupShaderBinding(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto found = g_bindings.find(object);
  return found != g_bindings.end() ? found->second : nullptr;
}

std::span<const uint8_t> ShaderDxil(const ShaderCacheEntry* entry) {
  if (!entry || entry->dxilSize == 0) return {};
  LoadBlobs();
  if (size_t(entry->dxilOffset) + entry->dxilSize > g_dxil.size()) return {};
  return {g_dxil.data() + entry->dxilOffset, entry->dxilSize};
}

std::span<const uint8_t> ShaderSpirv(const ShaderCacheEntry* entry) {
  if (!entry || entry->spirvSize == 0) return {};
  LoadBlobs();
  if (size_t(entry->spirvOffset) + entry->spirvSize > g_spirv_smolv.size()) return {};
  std::lock_guard<std::mutex> lock(g_mutex);
  auto [found, inserted] = g_spirv.try_emplace(entry);
  if (inserted) {
    const uint8_t* smolv = g_spirv_smolv.data() + entry->spirvOffset;
    found->second.resize(smolv::GetDecodedBufferSize(smolv, entry->spirvSize));
    if (!smolv::Decode(smolv, entry->spirvSize, found->second.data(), found->second.size())) found->second.clear();
  }
  return found->second;
}

}  // namespace cherry::gpu
