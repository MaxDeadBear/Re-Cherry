#include "gpu/hooks/hooks_internal.h"

#include <algorithm>
#include <cstring>

#include <rex/hook.h>

#include "gpu/guest_memory.h"
#include "gpu/shaders/shader_registry.h"

// The D3D binding setters UE3 still calls, written natively: bound objects, fetch constants, shader constants and the destination packet, with no ring packets.

namespace {

using namespace cherry::gpu;
using namespace cherry::gpu::hooks;

constexpr uint32_t kCpuFence = 0x2B1C;
constexpr uint32_t kStreams = 0x3224;
constexpr uint32_t kStreamStrides = 0x3268;
constexpr uint32_t kStreamStridesDrawn = 0x2FE8;
constexpr uint32_t kIndexBuffer = 0x320C;
constexpr uint32_t kRenderTargets = 0x3210;
constexpr uint32_t kDepthStencil = 0x3220;
constexpr uint32_t kTextures = 0x3278;
constexpr uint32_t kPixelShader = 0x330C;
constexpr uint32_t kVertexShader = 0x3310;
constexpr uint32_t kColorWriteEnables = 0x3004;
constexpr uint32_t kHighPrecisionBlend = 0x3078;
constexpr uint32_t kTilingTargets = 0x3330;
constexpr uint32_t kDefaultScissor = 0x821A5588;
constexpr uint32_t kDefaultViewport = 0x821A5570;

// An object leaving a slot takes the current CPU fence, so a Lock waits until the GPU is past its last use; with no guest GPU the fence is always published.
void StampReleasedFence(uint32_t device, uint32_t resource) {
  if (!resource) return;
  if (const uint32_t fence = LoadGuestU32(device + kCpuFence)) StoreGuestU32(resource + 8, fence);
}

bool TargetsMatchTilingSnapshot(uint32_t device, uint8_t flags) {
  if (flags & 0x10) return true;
  if (!(flags & 0x20)) return false;
  for (uint32_t i = 0; i < 5; ++i) {
    const uint32_t current = LoadGuestU32(device + kRenderTargets + 4 * i);
    if (current && current != LoadGuestU32(device + kTilingTargets + 4 * i)) return false;
  }
  return true;
}

// Bit 0 of byte dev+0x2B3C: predicated tiling is set up for exactly the bound targets.
void UpdateTilingFlag(uint32_t device) {
  uint8_t* base = GuestBase();
  const uint8_t flags = REX_LOAD_U8(device + 0x2B3C);
  const bool on = !LoadGuestU32(device + 0x3324) && !(flags & 0x0C) && !REX_LOAD_U8(device + 0x3113) && TargetsMatchTilingSnapshot(device, flags);
  REX_STORE_U8(device + 0x2B3C, uint8_t((flags & ~1u) | (on ? 1 : 0)));
}

// sub_829E0A50: a new first surface sets SurfaceInfo and the anti-aliasing config, and resets the scissor and viewport to the whole surface.
void SetSurfaceInfo(uint32_t device, uint32_t surface) {
  const uint32_t info = LoadGuestU32(surface + 0x18);
  const uint32_t msaa = (info >> 16) & 3;
  StoreGuestU32(device + 0x29BC, msaa == 0 ? 0 : msaa == 1 ? 0x8001 : 0xC003);
  StoreGuestU32(device + 0x2880, info);
  OrGuestU32(device + 0x10, 0x02000000);
  OrGuestU32(device + 0x20, 0x10);
  for (uint32_t i = 0; i < 4; ++i) StoreGuestU32(device + 0x32FC + 4 * i, LoadGuestU32(kDefaultScissor + 4 * i));
  SetGuestViewport(device, float(LoadGuestU32(kDefaultViewport)), float(LoadGuestU32(kDefaultViewport + 4)), float(LoadGuestU32(kDefaultViewport + 8)), float(LoadGuestU32(kDefaultViewport + 12)), LoadGuestF32(kDefaultViewport + 16), LoadGuestF32(kDefaultViewport + 20));
}

// UE3 passes PendingMask3 as 0x80000000 >> ((95 - n) / 3), always in the low word.
void D3DDevice_SetStreamSource_hook(uint32_t device, uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride, uint32_t pending) {
  uint8_t* base = GuestBase();
  if (buffer) {
    const uint32_t fc = device + 0x778 - 8 * stream;
    StoreGuestU32(fc, GuestPhysicalAddress(LoadGuestU32(buffer + 0x18) + offset));
    StoreGuestU32(fc + 4, LoadGuestU32(buffer + 0x1C) - offset);
    OrGuestU32(device + 0x1C, pending);
  }
  const uint32_t slot = device + kStreams + 4 * stream;
  StampReleasedFence(device, LoadGuestU32(slot));
  StoreGuestU32(slot, buffer);
  REX_STORE_U8(device + kStreamStrides + stream, uint8_t(stride >> 2));
  if ((stride >> 2) && (stride >> 2) != REX_LOAD_U8(device + kStreamStridesDrawn + stream)) OrGuestU32(device + 0x14, 0x00080000);
}

void D3DDevice_SetIndices_hook(uint32_t device, uint32_t indices) {
  StampReleasedFence(device, LoadGuestU32(device + kIndexBuffer));
  StoreGuestU32(device + kIndexBuffer, indices);
}

bool IsHighPrecisionCapable(uint32_t format) {
  return format == 2 || format == 3 || format == 10 || format == 12;
}

void D3DDevice_SetRenderTarget_hook(uint32_t device, uint32_t index, uint32_t surface) {
  uint8_t* base = GuestBase();
  StoreGuestU32(device + kRenderTargets + 4 * index, surface);
  if (surface) {
    // DepthInfo sits between Color0Info and Color1Info.
    const uint32_t slot = index ? index + 1 : 0;
    const uint32_t info_address = device + 0x2884 + 4 * slot;
    const uint32_t info = LoadGuestU32(surface + 0x1C);
    StoreGuestU32(info_address, info);
    OrGuestU32(device + 0x10, 0x01000000u >> slot);
    const uint32_t high_precision = LoadGuestU32(device + kHighPrecisionBlend + 4 * index);
    const uint32_t format = (info >> 16) & 0xF;
    if (IsHighPrecisionCapable(format) && (((info >> 19) & 1) ^ high_precision)) {
      const uint32_t m = high_precision - 1;
      const uint32_t swapped = ((~m << 16) & ((format + 3) << 17)) | ((((format >> 1) - 3) & m) << 16);
      StoreGuestU32(info_address, (info & 0xFFF0FFFFu) | (swapped & 0x000F0000u));
      OrGuestU32(device + 0x10, 0x01000000u >> slot);
    }
  }
  if (index == 0) {
    if (!surface) {
      const uint32_t color0 = LoadGuestU32(device + 0x2884);
      const uint32_t format = (color0 >> 16) & 0xF;
      if (format == 5 || format == 7 || format == 15) {
        StoreGuestU32(device + 0x2884, color0 & 0xFFF0FFFFu);
        OrGuestU32(device + 0x10, 0x01000000);
      }
    }
    const uint8_t flags = REX_LOAD_U8(device + 0x2B3F) & 0xF7;
    REX_STORE_U8(device + 0x2B3F, flags);
    uint32_t target = surface;
    if (surface) {
      const uint32_t format = REX_LOAD_U16(surface + 0x1C) & 0xF;
      if (format == 5 || format == 7 || format == 15) {
        REX_STORE_U8(device + 0x2B3F, uint8_t(flags | 8));
        OrGuestU32(device + 0x14, 0x00100000);
      }
    } else {
      target = LoadGuestU32(device + kDepthStencil);
    }
    if (target) SetSurfaceInfo(device, target);
    UpdateTilingFlag(device);
  }
  if (index <= 3) {
    const uint32_t write = LoadGuestU32(device + kRenderTargets + 4 * index) ? LoadGuestU32(device + kColorWriteEnables + 4 * index) : 0;
    const uint32_t shift = 4 * index;
    StoreGuestU32(device + 0x28DC, (LoadGuestU32(device + 0x28DC) & ~(0xFu << shift)) | ((write & 0xF) << shift));
    OrGuestU32(device + 0x10, 0x20);
  }
}

void D3DDevice_SetDepthStencilSurface_hook(uint32_t device, uint32_t surface) {
  uint8_t* base = GuestBase();
  StoreGuestU32(device + kDepthStencil, surface);
  if (surface) {
    if (!LoadGuestU32(device + kRenderTargets)) SetSurfaceInfo(device, surface);
    StoreGuestU32(device + 0x2888, LoadGuestU32(surface + 0x1C));
    const uint32_t hi_control = LoadGuestU32(surface + 0x20);
    StoreGuestU32(device + 0x2940, hi_control);
    REX_STORE_U8(device + 0x2B3E, uint8_t((REX_LOAD_U8(device + 0x2B3E) & ~0x20) | ((hi_control & 1) << 5)));
    if ((REX_LOAD_U8(device + 0x2B3F) & 0x20) && TargetsMatchTilingSnapshot(device, REX_LOAD_U8(device + 0x2B3C))) StoreGuestU32(device + 0x2880, (LoadGuestU32(device + 0x2880) & 0x0003FFFF) | ((LoadGuestU32(device + 0x34EC) & 0x3FFF) << 18));
    OrGuestU32(device + 0x10, 0x00800000);
    OrGuestU32(device + 0x14, 0x00020100);
  } else {
    StoreGuestU32(device + 0x2940, LoadGuestU32(device + 0x2940) & ~0xFu);
    REX_STORE_U8(device + 0x2B3E, uint8_t(REX_LOAD_U8(device + 0x2B3E) & 0xDF));
    OrGuestU32(device + 0x14, 0x100);
  }
  UpdateTilingFlag(device);
  // DepthControl's ZEnable and StencilEnable follow the render states (0x3014, 0x3018) only while a depth surface is bound.
  const bool bound = LoadGuestU32(device + kDepthStencil) != 0;
  const uint32_t z_enable = bound ? LoadGuestU32(device + 0x3014) : 0;
  const uint32_t stencil_enable = bound ? LoadGuestU32(device + 0x3018) : 0;
  StoreGuestU32(device + 0x2934, (LoadGuestU32(device + 0x2934) & ~3u) | ((z_enable & 1) << 1) | (stencil_enable & 1));
  OrGuestU32(device + 0x14, 0x00020800);
}

// UE3 passes PendingMask3 as 0x80000000 >> sampler, in the low word. The texture's GPUTEXTURE_FETCH_CONSTANT becomes the sampler's, keeping the sampler-state fields (clamps, filters, mip clamps against the sampler's own limits) and converting both addresses to physical.
void D3DDevice_SetTexture_hook(uint32_t device, uint32_t sampler, uint32_t texture, uint32_t pending) {
  uint8_t* base = GuestBase();
  const uint32_t fc = device + 0x480 + 24 * sampler;
  const uint32_t slot = device + kTextures + 4 * sampler;
  const uint32_t old = LoadGuestU32(slot);
  if (texture) {
    uint32_t t[6], f[6];
    for (uint32_t i = 0; i < 6; ++i) {
      t[i] = LoadGuestU32(texture + 0x1C + 4 * i);
      f[i] = LoadGuestU32(fc + 4 * i);
    }
    f[0] = (t[0] & ~0x003FFC00u) | (f[0] & 0x003FFC00u);
    f[1] = (GuestPhysicalAddress(t[1]) & ~0x800u) | (f[1] & 0x800u);
    f[2] = t[2];
    f[3] = (t[3] & ~0x7FF80000u) | (f[3] & 0x7FF80000u);
    const uint32_t min_mip = std::max<uint32_t>((t[4] >> 2) & 0xF, REX_LOAD_U8(device + 0x3044 + sampler));
    const uint32_t max_mip = std::min<uint32_t>((t[4] >> 6) & 0xF, REX_LOAD_U8(device + 0x305E + sampler));
    f[4] = (f[4] & 0xFFFFFC03u) | ((min_mip & 0xF) << 2) | ((max_mip & 0xF) << 6);
    f[5] = ((t[5] & 0x1FFFFE00u) + (((t[5] >> 20) + 0x200) & 0x1000)) | (f[5] & 0x1FFu);
    for (uint32_t i = 0; i < 6; ++i) StoreGuestU32(fc + 4 * i, f[i]);
    OrGuestU32(device + 0x1C, pending);
  } else {
    StoreGuestU32(fc, LoadGuestU32(fc) & ~3u);
  }
  StoreGuestU32(slot, texture);
  StampReleasedFence(device, old);
}

// The PendingMask argument spans both words of r7, and typed hooks receive only the low one, so it is rebuilt the way every UE3 caller computes it: one bit per group of four registers touched.
uint64_t ConstantPendingMask(uint32_t start, uint32_t count) {
  const uint32_t first = start >> 2, span = (((start + count - 1) >> 2) - first) & 0x7F;
  const uint64_t ones = span >= 64 ? ~0ull : uint64_t(int64_t(0x8000000000000000ull) >> span);
  return (first & 0x7F) >= 64 ? 0 : ones >> (first & 0x7F);
}

void SetShaderConstants(uint32_t device, uint32_t constants, uint32_t pending_address, uint32_t start, uint32_t data, uint32_t count) {
  uint8_t* base = GuestBase();
  std::memmove(base + ((device + constants + 16 * start) & ~0xFu), base + data, size_t(count) * 16);
  OrGuestU64(device + pending_address, ConstantPendingMask(start, count));
}

void D3DDevice_SetVertexShaderConstantFN_hook(uint32_t device, uint32_t start, uint32_t data, uint32_t count) {
  SetShaderConstants(device, 0x780, 0x0, start, data, count);
}

void D3DDevice_SetPixelShaderConstantFN_hook(uint32_t device, uint32_t start, uint32_t data, uint32_t count) {
  SetShaderConstants(device, 0x1780, 0x8, start, data, count);
}

// A shader's patch block: pending bits it satisfies, constant records copied into the device, then AND/OR masks applied to device registers.
void ApplyShaderPatches(uint32_t device, uint32_t container, uint32_t pending_address) {
  uint8_t* base = GuestBase();
  const uint32_t offset = LoadGuestU32(container + 0x14);
  if (!offset) return;
  const uint32_t block = container + offset;
  StoreGuestU32(device + pending_address, LoadGuestU32(device + pending_address) & ~LoadGuestU32(block));
  StoreGuestU32(device + pending_address + 4, LoadGuestU32(device + pending_address + 4) & ~LoadGuestU32(block + 4));
  if (LoadGuestU32(block + 8) | LoadGuestU32(block + 12)) OrGuestU32(device + 0x20, 0x01000000);
  const uint32_t registers = device + 0x480;
  uint32_t p = block + 0x14;
  const uint32_t end = p + LoadGuestU32(block + 0x10);
  while (p < end) {
    const uint32_t count = REX_LOAD_U16(p + 2);
    p += 4;
    if (!count) break;
    p += 4;
  }
  bool masks = false;
  while (p < end) {
    const uint32_t at = REX_LOAD_U16(p), count = REX_LOAD_U16(p + 2);
    p += 4;
    if (!count) {
      masks = true;
      break;
    }
    std::memmove(base + registers + at, base + p, size_t(count) * 4);
    p += count * 4;
  }
  if (!masks) return;
  while (p < end) {
    const uint32_t at = REX_LOAD_U16(p);
    uint32_t count = REX_LOAD_U16(p + 2);
    p += 4;
    if (!count) return;
    for (uint32_t target = registers + at; count; count = (count - 2) & 0xFFFF, p += 8, target += 4) StoreGuestU32(target, (LoadGuestU32(target) & LoadGuestU32(p)) | LoadGuestU32(p + 4));
  }
}

void D3DDevice_SetPixelShader_hook(uint32_t device, uint32_t shader) {
  StampReleasedFence(device, LoadGuestU32(device + kPixelShader));
  StoreGuestU32(device + kPixelShader, shader);
  OrGuestU32(device + 0x14, 0x00120000);
  if (shader) ApplyShaderPatches(device, shader + 0x28, 0x8);
}

void D3DDevice_SetVertexShader_hook(uint32_t device, uint32_t shader) {
  uint8_t* base = GuestBase();
  if (shader) OrGuestU32(device + 0x14, 0x00080000);
  StampReleasedFence(device, LoadGuestU32(device + kVertexShader));
  REX_STORE_U8(device + 0x2B3E, uint8_t(REX_LOAD_U8(device + 0x2B3E) & 0x7F));
  StoreGuestU32(device + kVertexShader, shader);
  if (shader) ApplyShaderPatches(device, shader + 0x368, 0x0);
}

void XGSetVertexDeclarationHeader_hook(uint32_t elements, uint32_t declaration) {
  uint8_t* base = GuestBase();
  uint8_t used[16] = {};
  uint32_t count = 0, max_stream = 0;
  for (uint32_t e = elements;; e += 12) {
    const uint32_t stream = REX_LOAD_U16(e);
    if (stream == 0xFF) break;
    max_stream = std::max(max_stream, stream);
    if (stream < 16) used[stream] = 0xFF;
    ++count;
  }
  std::memset(base + declaration, 0, count * 12 + 0x38);
  StoreGuestU32(declaration + 0x00, 0x00100005);
  StoreGuestU32(declaration + 0x04, 1);
  StoreGuestU32(declaration + 0x14, 0xFFFF0000);
  StoreGuestU32(declaration + 0x18, count);
  StoreGuestU32(declaration + 0x1C, max_stream);
  std::memcpy(base + declaration + 0x20, used, sizeof(used));
  std::memmove(base + declaration + 0x34, base + elements, count * 12);
}

// Copies each variant's vertex-fetch instructions next to the object, where D3DVertexShader_Bind's bookkeeping lives; the declaration id starts unbound. XeVertexShader_Create copies the container's virtual part to +0x368 first, so the shader is registered here.
void XGRegisterVertexShader_hook(uint32_t shader, uint32_t physical) {
  uint8_t* base = GuestBase();
  std::memset(base + shader, 0, 0x368);
  StoreGuestU32(shader + 0x20, physical);
  StoreGuestU32(shader + 0x00, 6);
  StoreGuestU32(shader + 0x04, 1);
  StoreGuestU32(shader + 0x14, 0xFFFF0000);
  const uint32_t container = shader + 0x368;
  const uint32_t variants = (LoadGuestU32(container) & 0x20) ? 2 : 1;
  for (uint32_t v = 0; v < variants; ++v) {
    const uint32_t header = container + LoadGuestU32(container + 0x18 + 8 * v);
    const uint32_t code = physical + LoadGuestU32(header);
    const uint32_t count = LoadGuestU32(header + 0x1C);
    const uint32_t table = header + 0x24 + 4 * LoadGuestU32(header + 0x18);
    uint32_t destination = shader + 0x44 + 0x1A0 * v;
    for (uint32_t i = 0; i < count; ++i, destination += 12) {
      const uint32_t source = code + 12 * (LoadGuestU32(table + 4 * i) & 0xFFF);
      if (source != destination) std::memmove(base + destination, base + source, 12);
    }
    StoreGuestU32(shader + 0x28 + 0x1A0 * v, 0xFFFFFFFF);
  }
  RegisterShader(base, container, physical, shader, false);
}

void XGRegisterPixelShader_hook(uint32_t shader, uint32_t physical) {
  std::memset(GuestBase() + shader, 0, 0x28);
  StoreGuestU32(shader + 0x00, 7);
  StoreGuestU32(shader + 0x04, 1);
  StoreGuestU32(shader + 0x14, 0xFFFF0000);
  StoreGuestU32(shader + 0x18, physical);
  // XePixelShader_Create copies the container's virtual part to +0x28 first.
  RegisterShader(GuestBase(), shader + 0x28, physical, shader, true);
}

// Ring data comes from the custom allocator's fixed buffer that SetRingBufferParameters installs, carved down toward the command write pointer.
uint32_t D3D_CDevice_AllocateRingData_hook(uint32_t device, uint32_t dwords, uint32_t alignment) {
  uint8_t* base = GuestBase();
  if (REX_LOAD_U8(device + 0x2B3D) & 0x20) return 0;
  const uint32_t bytes = dwords << 2;
  const uint32_t allocator = LoadGuestU32(device + 0x3660);
  const uint32_t buffer = allocator ? LoadGuestU32(allocator + 0x98) : 0;
  uint32_t result = 0;
  if (buffer) {
    uint32_t cursor = LoadGuestU32(device + 0x3BD8);
    if (!cursor) cursor = (LoadGuestU32(allocator + 0x9C) & ~3u) + buffer;
    const uint32_t p = (cursor - (bytes & ~3u)) & ~(alignment - 1);
    if (p >= LoadGuestU32(device + 0x30) + 0xA4) {
      const uint32_t moved = uint32_t(int32_t(cursor - p) >> 2) << 2;
      StoreGuestU32(device + 0x3BD8, p);
      StoreGuestU32(device + 0x34, LoadGuestU32(device + 0x34) - moved);
      StoreGuestU32(device + 0x38, LoadGuestU32(device + 0x38) - moved);
      result = p;
    }
  }
  const uint32_t used = LoadGuestU32(device + 0x3BE8) + bytes;
  StoreGuestU32(device + 0x3BE8, used);
  if (used > LoadGuestU32(device + 0x3BEC) && !LoadGuestU32(device + 0x3520)) {
    const uint32_t write = LoadGuestU32(device + 0x30);
    StoreGuestU32(device + 0x34, write + 0xA0);
    StoreGuestU32(device + 0x38, write);
    if (LoadGuestU32(device + 0x3BE0) != LoadGuestU32(device + 0x3BCC)) StoreGuestU32(device + 0x3BD8, LoadGuestU32(device + 0x3BD0));
  }
  if (!result) REX_STORE_U8(device + 0x2B3D, uint8_t(REX_LOAD_U8(device + 0x2B3D) | 0x20));
  return result;
}

}  // namespace

// D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*, UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3)
REX_HOOK(D3DDevice_SetStreamSource, D3DDevice_SetStreamSource_hook)
// D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*)
REX_HOOK(D3DDevice_SetIndices, D3DDevice_SetIndices_hook)
// D3DDevice_SetRenderTarget(D3DDevice*, DWORD RenderTargetIndex, D3DSurface*)
REX_HOOK(D3DDevice_SetRenderTarget, D3DDevice_SetRenderTarget_hook)
// D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*)
REX_HOOK(D3DDevice_SetDepthStencilSurface, D3DDevice_SetDepthStencilSurface_hook)
// D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*, UINT64 PendingMask3)
REX_HOOK(D3DDevice_SetTexture, D3DDevice_SetTexture_hook)
// D3DDevice_SetVertexShaderConstantFN(D3DDevice*, UINT StartRegister, const float*, DWORD Vector4fCount, UINT64 PendingMask0)
REX_HOOK(D3DDevice_SetVertexShaderConstantFN, D3DDevice_SetVertexShaderConstantFN_hook)
// D3DDevice_SetPixelShaderConstantFN(D3DDevice*, UINT StartRegister, const float*, DWORD Vector4fCount, UINT64 PendingMask1)
REX_HOOK(D3DDevice_SetPixelShaderConstantFN, D3DDevice_SetPixelShaderConstantFN_hook)
// D3DDevice_SetPixelShader(D3DDevice*, D3DPixelShader*)
REX_HOOK(D3DDevice_SetPixelShader, D3DDevice_SetPixelShader_hook)
// D3DDevice_SetVertexShader(D3DDevice*, D3DVertexShader*)
REX_HOOK(D3DDevice_SetVertexShader, D3DDevice_SetVertexShader_hook)
// XGSetVertexDeclarationHeader(const D3DVERTEXELEMENT9*, D3DVertexDeclaration*)
REX_HOOK(XGSetVertexDeclarationHeader, XGSetVertexDeclarationHeader_hook)
// XGRegisterVertexShader(D3DVertexShader*, const void* pPhysicalPart)
REX_HOOK(XGRegisterVertexShader, XGRegisterVertexShader_hook)
// XGRegisterPixelShader(D3DPixelShader*, const void* pPhysicalPart)
REX_HOOK(XGRegisterPixelShader, XGRegisterPixelShader_hook)
// D3D_CDevice_AllocateRingData(D3DDevice*, DWORD Dwords, DWORD Alignment) -> void*
REX_HOOK(D3D_CDevice_AllocateRingData, D3D_CDevice_AllocateRingData_hook)
