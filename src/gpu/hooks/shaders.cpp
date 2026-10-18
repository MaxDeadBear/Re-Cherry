#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

#include "re_cherry_init.h"

#include <rex/hook.h>

#include "gpu/guest_memory.h"
#include "gpu/shaders/shader_registry.h"

namespace {

// D3DVertexShader_Bind's bookkeeping (0x829EE1B0): the declaration's id at +48, drawn from a global counter, and per microcode variant the id at +40 and the strides at +48, variants 416 bytes apart, with the container flags at +872 (bit 0x20 a second variant, 0x40 bound).
constexpr uint32_t kDeclarationIdCounter = 0x8337602C;
constexpr uint32_t kDeclarationMaxStream = 28;
constexpr uint32_t kDeclarationId = 48;
constexpr uint32_t kShaderBoundDeclarationId = 40;
constexpr uint32_t kShaderBoundStrides = 48;
constexpr uint32_t kShaderVariantBytes = 416;
constexpr uint32_t kShaderContainerFlags = 872;

uint32_t NextDeclarationId() {
  auto& counter = *reinterpret_cast<std::atomic<uint32_t>*>(cherry::gpu::GuestBase() + kDeclarationIdCounter);
  uint32_t stored = counter.load();
  for (;;) {
    uint32_t id = std::byteswap(stored) + 1;
    // The original skips 0 and 0xFFFFFFFF.
    if (id + 1 < 2) id = 1;
    if (counter.compare_exchange_weak(stored, std::byteswap(id))) return id;
  }
}

// Records the declaration and strides for the draw path and keeps the guest objects' bookkeeping; the microcode patch is skipped, since nothing executes guest microcode.
void D3DVertexShader_Bind_hook(uint32_t vertex_shader, uint32_t declaration, uint32_t strides, uint32_t pixel_shader) {
  (void)pixel_shader;
  using namespace cherry::gpu;
  if (!vertex_shader || !declaration) return;
  VertexShaderBinding binding;
  binding.declaration = declaration;
  const uint32_t max_stream = std::min(LoadGuestU32(declaration + kDeclarationMaxStream), 15u);
  for (uint32_t s = 0; strides && s <= max_stream; ++s) binding.strides[s] = uint8_t(LoadGuestU32(strides + 4 * s) >> 2);
  NoteVertexShaderBinding(vertex_shader, binding);
  uint32_t id = LoadGuestU32(declaration + kDeclarationId);
  if (!id) {
    id = NextDeclarationId();
    StoreGuestU32(declaration + kDeclarationId, id);
  }
  const uint32_t flags = LoadGuestU32(vertex_shader + kShaderContainerFlags);
  const uint32_t variants = (flags & 0x20) ? 2 : 1;
  for (uint32_t v = 0; v < variants; ++v) {
    std::memcpy(GuestBase() + vertex_shader + kShaderBoundStrides + v * kShaderVariantBytes, binding.strides, sizeof(binding.strides));
    StoreGuestU32(vertex_shader + kShaderBoundDeclarationId + v * kShaderVariantBytes, id);
  }
  StoreGuestU32(vertex_shader + kShaderContainerFlags, flags | 0x40);
}

}  // namespace

// D3DVertexShader_Bind(D3DVertexShader*, D3DVertexDeclaration*, const DWORD* strides, D3DPixelShader*)
REX_HOOK(D3DVertexShader_Bind, D3DVertexShader_Bind_hook)
