// Binds the guest's D3D shader objects to translated cache entries, and dumps the containers the cache is built from.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

struct ShaderCacheEntry;

namespace cherry::gpu {

// Where shader containers are dumped: the shader_dump folder beside the game's assets folder, so a dev build dumps into the repository.
void SetShaderDumpDirectory(const std::filesystem::path& directory);

// Called from XGRegister*Shader with the two halves of a 0x102A11xx container: the virtual part UE3 copied into `object` and the physical part (microcode) it copied elsewhere.
void RegisterShader(uint8_t* base, uint32_t virtual_part, uint32_t physical_part, uint32_t object, bool pixel_shader);

// The vertex declaration and per-stream strides (in dwords) a vertex shader object was last bound with by D3DVertexShader_Bind; UE3 binds through it instead of setting a declaration on the device.
struct VertexShaderBinding {
  uint32_t declaration = 0;
  uint8_t strides[16] = {};
};
void NoteVertexShaderBinding(uint32_t object, const VertexShaderBinding& binding);
bool LookupVertexShaderBinding(uint32_t object, VertexShaderBinding* binding);

// The cache entry a bound guest shader object was created from, or null when it missed.
const ShaderCacheEntry* LookupShaderBinding(uint32_t object);

// The translated blobs for an entry, decompressed on first use; empty when the entry has none for that backend.
std::span<const uint8_t> ShaderDxil(const ShaderCacheEntry* entry);
std::span<const uint8_t> ShaderSpirv(const ShaderCacheEntry* entry);

}  // namespace cherry::gpu
