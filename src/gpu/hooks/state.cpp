#include "gpu/hooks/hooks_internal.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <initializer_list>

#include <rex/hook.h>

#include "gpu/guest_memory.h"

// The D3D state setters UE3 still calls, written natively: each makes the device-shadow and cache writes its recompiled body made, and none of its ring packets.

namespace {

using namespace cherry::gpu;

// Device fields the setters keep beside the hardware packets.
constexpr uint32_t kBlendCache = 0x2FF8;       // GPU_BLENDCONTROL as last set, colour factors only
constexpr uint32_t kBlendFlags = 0x2FFC;       // bit 31 AlphaBlendEnable, bit 30 SeparateAlphaBlendEnable
constexpr uint32_t kScissorTestEnable = 0x3000;
constexpr uint32_t kMaxMipLevels = 0x3044;     // byte per sampler
constexpr uint32_t kMinMipLevels = 0x305E;     // byte per sampler
constexpr uint32_t kHighPrecisionBlend = 0x3078;
constexpr uint32_t kViewportCache = 0x32E0;    // X, Y, W, H, MinZ, MaxZ floats, then a dword
constexpr uint32_t kScissorCache = 0x32FC;     // RECT
constexpr uint32_t kGprAllocation = 0x2B28;
constexpr uint32_t kGammaRamp = 0x3C30;        // 768 u16
constexpr uint32_t kDisplayFlags = 0x42DC;
constexpr uint32_t kMaxAnisotropy = 0x2A70;    // byte per sampler
constexpr uint32_t kZFilter = 0x2A8A;          // byte per sampler
constexpr uint32_t kAnisoFilterTable = 0x821A5598;

// PPC fctiwz: truncate and saturate, NaN to INT32_MIN.
int32_t Fctiwz(double v) {
  if (!(v > -2147483649.0)) return INT32_MIN;
  if (v >= 2147483648.0) return INT32_MAX;
  return static_cast<int32_t>(v);
}

// The low word of PPC fctidz.
uint32_t FctidzLow(double v) {
  if (std::isnan(v) || v < -9223372036854775808.0) return 0;
  if (v >= 9223372036854775808.0) return 0xFFFFFFFFu;
  return static_cast<uint32_t>(static_cast<int64_t>(v));
}

// Copies the colour factors and op into the alpha ones, as the setters do when separate alpha blending is off.
uint32_t ReplicateColorBlendToAlpha(uint32_t v) {
  return ((((v & 0x1010u) | ((v << 4) & 0xFFFF0u)) << 12) & 0xEFEF0000u) | (v & 0xFFFFu);
}

void WriteBlendControls(uint32_t device, uint32_t blend) {
  for (uint32_t offset : {0x2938u, 0x2958u, 0x295Cu, 0x2960u}) StoreGuestU32(device + offset, blend);
  OrGuestU32(device + 0x14, 0x407);
}

uint32_t FetchConstant(uint32_t device, uint32_t sampler) {
  return device + 0x480 + 24 * sampler;
}

void MarkFetchDirty(uint32_t device, uint32_t sampler) {
  OrGuestU32(device + 0x1C, 0x80000000u >> sampler);
}

// VolMagFilter and VolMinFilter follow the 2D filters unless m_ZFilter forces them.
uint32_t ApplyVolumeFilters(uint32_t device, uint32_t sampler, uint32_t d3, uint32_t d4) {
  uint8_t* base = GuestBase();
  const uint32_t z_filter = REX_LOAD_U8(device + kZFilter + sampler);
  const uint32_t derived = ((d3 >> 19) & 1) | ((d3 >> 20) & 2);
  const uint32_t mask = (z_filter >> 2) - 1;
  return (d4 & 0xFFFFFFFCu) | (((derived & mask) + (z_filter & ~mask)) & 3);
}

void D3DDevice_SetRenderState_AlphaBlendEnable_hook(uint32_t device, uint32_t value) {
  const uint32_t flags = (LoadGuestU32(device + kBlendFlags) & 0x7FFFFFFFu) | (value << 31);
  StoreGuestU32(device + kBlendFlags, flags);
  uint32_t blend = LoadGuestU32(device + kBlendCache);
  if (!(flags & 0x40000000u)) blend = ReplicateColorBlendToAlpha(blend);
  if (value == 0) blend = 0x00010001u;
  WriteBlendControls(device, blend);
}

void SetBlendFactor(uint32_t device, uint32_t mask, uint32_t field) {
  const uint32_t flags = LoadGuestU32(device + kBlendFlags);
  const uint32_t blend = (LoadGuestU32(device + kBlendCache) & ~mask) | field;
  StoreGuestU32(device + kBlendCache, blend);
  if (flags & 0x80000000u) WriteBlendControls(device, (flags & 0x40000000u) ? blend : ReplicateColorBlendToAlpha(blend));
}

void D3DDevice_SetRenderState_SrcBlend_hook(uint32_t device, uint32_t value) {
  SetBlendFactor(device, 0x1F, value & 0x1F);
}

void D3DDevice_SetRenderState_DestBlend_hook(uint32_t device, uint32_t value) {
  SetBlendFactor(device, 0x1F00, (value & 0x1F) << 8);
}

void D3DDevice_SetRenderState_SeparateAlphaBlendEnable_hook(uint32_t device, uint32_t value) {
  const uint32_t flags = (LoadGuestU32(device + kBlendFlags) & 0xBFFFFFFFu) | ((value & 1) << 30);
  StoreGuestU32(device + kBlendFlags, flags);
  uint32_t blend = LoadGuestU32(device + kBlendCache);
  if (value == 0) blend = ReplicateColorBlendToAlpha(blend);
  if (!(flags >> 31)) blend = 0x00010001u;
  WriteBlendControls(device, blend);
}

// ModeControl bits 11 and 12 enable the front and back polygon offset whenever either term is set.
void SetPolygonOffsetEnables(uint32_t device, bool front, bool back) {
  StoreGuestU32(device + 0x2948, (LoadGuestU32(device + 0x2948) & ~0x1800u) | (uint32_t(front) << 11) | (uint32_t(back) << 12));
}

void D3DDevice_SetRenderState_SlopeScaleDepthBias_hook(uint32_t device, uint32_t value) {
  const float scale = std::bit_cast<float>(value) * 16.0f;
  SetPolygonOffsetEnables(device, scale != 0.0f || LoadGuestF32(device + 0x2A54) != 0.0f, scale != 0.0f || LoadGuestF32(device + 0x2A5C) != 0.0f);
  StoreGuestF32(device + 0x2A50, scale);
  StoreGuestF32(device + 0x2A58, scale);
  OrGuestU32(device + 0x20, 0x2800);
  OrGuestU32(device + 0x14, 0x40);
}

void D3DDevice_SetRenderState_DepthBias_hook(uint32_t device, uint32_t value) {
  const float bias = std::bit_cast<float>(value);
  SetPolygonOffsetEnables(device, LoadGuestF32(device + 0x2A50) != 0.0f || bias != 0.0f, LoadGuestF32(device + 0x2A58) != 0.0f || bias != 0.0f);
  StoreGuestU32(device + 0x2A54, value);
  StoreGuestU32(device + 0x2A5C, value);
  OrGuestU32(device + 0x20, 0x1400);
  OrGuestU32(device + 0x14, 0x40);
}

// The 2-bit render-target formats 2 and 3 have high-precision twins 0xA and 0xC; the setter swaps between them for its target.
void SetHighPrecisionBlend(uint32_t device, uint32_t value, uint32_t target, uint32_t dirty) {
  StoreGuestU32(device + kHighPrecisionBlend + 4 * target, value);
  if (!LoadGuestU32(device + 0x3210 + 4 * target)) return;
  const uint32_t info_address = device + 0x2884 + 8 * target;
  const uint32_t info = LoadGuestU32(info_address);
  const uint32_t format = (info >> 16) & 0xF;
  if (format != 2 && format != 3 && format != 0xA && format != 0xC) return;
  if ((((info >> 19) & 1) ^ value) == 0) return;
  const uint32_t m = value - 1;
  const uint32_t swapped = ((~m << 16) & ((format + 3) << 17)) | ((((format >> 1) - 3) & m) << 16);
  StoreGuestU32(info_address, (info & 0xFFF0FFFFu) | (swapped & 0x000F0000u));
  OrGuestU32(device + 0x10, dirty);
}

void D3DDevice_SetRenderState_HighPrecisionBlendEnable_hook(uint32_t device, uint32_t value) {
  SetHighPrecisionBlend(device, value, 0, 0x01000000u);
}

void D3DDevice_SetRenderState_HighPrecisionBlendEnable1_hook(uint32_t device, uint32_t value) {
  SetHighPrecisionBlend(device, value, 1, 0x00400000u);
}

uint32_t AnisoFilter(uint32_t device, uint32_t sampler) {
  uint8_t* base = GuestBase();
  return LoadGuestU32(kAnisoFilterTable + 4 * REX_LOAD_U8(device + kMaxAnisotropy + sampler));
}

void D3DDevice_SetSamplerState_MinFilter_hook(uint32_t device, uint32_t sampler, uint32_t value) {
  const uint32_t fc = FetchConstant(device, sampler);
  uint32_t d3 = LoadGuestU32(fc + 0xC);
  uint32_t d4 = LoadGuestU32(fc + 0x10);
  const uint32_t walk = value >> 2;
  const uint32_t aniso = AnisoFilter(device, sampler) & ~((((d4 >> 10) & 1) | walk) - 1);
  const uint32_t f = (aniso << 4) | walk | value;
  d4 = (d4 & ~0x800u) | ((walk & 1) << 11);
  d3 = (d3 & ~0x0E600000u) | ((f & 3) << 21) | (((f >> 4) & 7) << 25);
  StoreGuestU32(fc + 0xC, d3);
  StoreGuestU32(fc + 0x10, ApplyVolumeFilters(device, sampler, d3, d4));
  MarkFetchDirty(device, sampler);
}

void D3DDevice_SetSamplerState_MagFilter_hook(uint32_t device, uint32_t sampler, uint32_t value) {
  const uint32_t fc = FetchConstant(device, sampler);
  uint32_t d3 = LoadGuestU32(fc + 0xC);
  uint32_t d4 = LoadGuestU32(fc + 0x10);
  const uint32_t walk = value >> 2;
  const uint32_t aniso = AnisoFilter(device, sampler) & ~((((d4 >> 11) & 1) | walk) - 1);
  const uint32_t f = (aniso << 6) | walk | value;
  d4 = (d4 & ~0x400u) | ((walk & 1) << 10);
  d3 = (d3 & ~0x0E180000u) | ((f & 3) << 19) | (((f >> 6) & 7) << 25);
  StoreGuestU32(fc + 0xC, d3);
  StoreGuestU32(fc + 0x10, ApplyVolumeFilters(device, sampler, d3, d4));
  MarkFetchDirty(device, sampler);
}

void D3DDevice_SetSamplerState_MaxAnisotropy_hook(uint32_t device, uint32_t sampler, uint32_t value) {
  uint8_t* base = GuestBase();
  const uint32_t fc = FetchConstant(device, sampler);
  if (LoadGuestU32(fc + 0x10) & 0xC00u) {
    StoreGuestU32(fc + 0xC, (LoadGuestU32(fc + 0xC) & ~0x0E000000u) | ((LoadGuestU32(kAnisoFilterTable + 4 * value) & 7) << 25));
    MarkFetchDirty(device, sampler);
  }
  REX_STORE_U8(device + kMaxAnisotropy + sampler, uint8_t(value));
}

void D3DDevice_SetSamplerState_MipMapLodBias_hook(uint32_t device, uint32_t sampler, uint32_t value) {
  const uint32_t fc = FetchConstant(device, sampler);
  const float bias = std::bit_cast<float>(value) * 32.0f;
  StoreGuestU32(fc + 0x10, (LoadGuestU32(fc + 0x10) & ~0x003FF000u) | ((FctidzLow(bias) & 0x3FF) << 12));
  MarkFetchDirty(device, sampler);
}

// The D3D sampler state named MaxMipLevel clamps the fetch constant's MinMipLevel field, and MinMipLevel its MaxMipLevel field.
void D3DDevice_SetSamplerState_MaxMipLevel_hook(uint32_t device, uint32_t sampler, uint32_t value) {
  uint8_t* base = GuestBase();
  if (const uint32_t texture = LoadGuestU32(device + 0x3278 + 4 * sampler)) {
    const uint32_t fc = FetchConstant(device, sampler);
    const uint32_t level = std::max((LoadGuestU32(texture + 0x2C) >> 2) & 0xF, value);
    StoreGuestU32(fc + 0x10, (LoadGuestU32(fc + 0x10) & ~0x3Cu) | ((level & 0xF) << 2));
    MarkFetchDirty(device, sampler);
  }
  REX_STORE_U8(device + kMaxMipLevels + sampler, uint8_t(value));
}

void D3DDevice_SetSamplerState_MinMipLevel_hook(uint32_t device, uint32_t sampler, uint32_t value) {
  uint8_t* base = GuestBase();
  if (const uint32_t texture = LoadGuestU32(device + 0x3278 + 4 * sampler)) {
    const uint32_t fc = FetchConstant(device, sampler);
    const uint32_t level = std::min((LoadGuestU32(texture + 0x2C) >> 6) & 0xF, value);
    StoreGuestU32(fc + 0x10, (LoadGuestU32(fc + 0x10) & ~0x3C0u) | ((level & 0xF) << 6));
    MarkFetchDirty(device, sampler);
  }
  REX_STORE_U8(device + kMinMipLevels + sampler, uint8_t(value));
}

void D3DDevice_SetScissorRect_hook(uint32_t device, uint32_t rect) {
  cherry::gpu::hooks::SetGuestScissorRect(device, rect);
}

void D3DDevice_SetViewport_hook(uint32_t device, uint32_t viewport) {
  cherry::gpu::hooks::SetGuestViewport(device, float(LoadGuestU32(viewport)), float(LoadGuestU32(viewport + 4)), float(LoadGuestU32(viewport + 8)), float(LoadGuestU32(viewport + 12)), LoadGuestF32(viewport + 0x10), LoadGuestF32(viewport + 0x14));
}

// The guest maps the ramp through the TV's display gamma from VdGetCurrentDisplayGamma; a PC display takes it as is, so only the change tracking Swap reads remains.
void D3DDevice_SetGammaRamp_hook(uint32_t device, uint32_t ramp) {
  uint8_t* base = GuestBase();
  bool changed = false;
  for (uint32_t i = 0; i < 768; ++i) {
    const uint16_t value = REX_LOAD_U16(ramp + 2 * i) & 0xFFC0;
    if (REX_LOAD_U16(device + kGammaRamp + 2 * i) == value) continue;
    REX_STORE_U16(device + kGammaRamp + 2 * i, value);
    changed = true;
  }
  if (changed) OrGuestU32(device + kDisplayFlags, 2);
}

void D3DDevice_SetShaderGPRAllocation_hook(uint32_t device, uint32_t flags, uint32_t vertex_gprs, uint32_t pixel_gprs) {
  if (vertex_gprs == 0 && pixel_gprs == 0) vertex_gprs = pixel_gprs = 64;
  uint32_t allocation = 0;
  if (!(flags & 1)) allocation = (LoadGuestU32(device + kGprAllocation) & ~0x0007F7F0u) | ((pixel_gprs & 0x7F) << 4) | ((vertex_gprs & 0x7F) << 12);
  StoreGuestU32(device + kGprAllocation, allocation);
  OrGuestU64(device + 0x10, 0x000013A0001809E8ull);
  OrGuestU64(device + 0x18, 0x0040000880000000ull);
  OrGuestU64(device + 0x20, 0x0000000800080000ull);
}

}  // namespace

namespace cherry::gpu::hooks {

void SetGuestScissorRect(uint32_t device, uint32_t rect) {
  const int32_t vx = Fctiwz(LoadGuestF32(device + kViewportCache));
  const int32_t vy = Fctiwz(LoadGuestF32(device + kViewportCache + 4));
  const int32_t vw = Fctiwz(LoadGuestF32(device + kViewportCache + 8));
  const int32_t vh = Fctiwz(LoadGuestF32(device + kViewportCache + 12));
  const int32_t l = int32_t(LoadGuestU32(rect)), t = int32_t(LoadGuestU32(rect + 4)), r = int32_t(LoadGuestU32(rect + 8)), b = int32_t(LoadGuestU32(rect + 12));
  for (uint32_t i = 0; i < 4; ++i) StoreGuestU32(device + kScissorCache + 4 * i, LoadGuestU32(rect + 4 * i));
  int32_t x0 = vx, y0 = vy;
  int32_t x1 = int32_t(uint32_t(vx) + uint32_t(vw)), y1 = int32_t(uint32_t(vy) + uint32_t(vh));
  if (LoadGuestU32(device + kScissorTestEnable)) {
    if (!(x0 > l)) x0 = l;
    if (!(y0 > t)) y0 = t;
    if (!(x1 < r)) x1 = r;
    if (!(y1 < b)) y1 = b;
  }
  StoreGuestU32(device + 0x28C4, (LoadGuestU32(device + 0x28C4) & 0x80008000u) | ((uint32_t(y0) & 0x7FFF) << 16) | (uint32_t(x0) & 0x7FFF));
  StoreGuestU32(device + 0x28C8, (LoadGuestU32(device + 0x28C8) & 0x80008000u) | ((uint32_t(y1) & 0x7FFF) << 16) | (uint32_t(x1) & 0x7FFF));
}

// The float viewport setter (0x829E0DC8): clipped to the bound surface, or to the tile bounds while predicated tiling covers the bound targets.
void SetGuestViewport(uint32_t device, float x, float y, float w, float h, float min_z, float max_z) {
  uint8_t* base = GuestBase();
  uint32_t surface = LoadGuestU32(device + 0x3210);
  if (!surface && !(surface = LoadGuestU32(device + 0x3220))) return;
  const uint8_t flags = REX_LOAD_U8(device + 0x2B3C);
  bool tiled = (flags & 0x10) != 0;
  if (!tiled && (flags & 0x20)) {
    tiled = true;
    for (uint32_t i = 0; i < 5 && tiled; ++i) {
      const uint32_t current = LoadGuestU32(device + 0x3210 + 4 * i);
      tiled = !current || LoadGuestU32(device + 0x3330 + 4 * i) == current;
    }
  }
  int32_t max_x, max_y;
  if (tiled) {
    max_x = int32_t(LoadGuestU32(device + 0x34F4));
    max_y = int32_t(LoadGuestU32(device + 0x34F8));
  } else {
    const uint32_t size = LoadGuestU32(surface + 0x24);
    max_x = int32_t(size >> 18) + 1;
    max_y = int32_t((size >> 3) & 0x7FFF) + 1;
  }
  const int32_t ix = Fctiwz(x), iw = Fctiwz(w), iy = Fctiwz(y), ih = Fctiwz(h);
  max_x = std::min(max_x, int32_t(uint32_t(ix) + uint32_t(iw)));
  max_y = std::min(max_y, int32_t(uint32_t(iy) + uint32_t(ih)));
  int32_t cw = int32_t(uint32_t(max_x) - uint32_t(ix)), ch = int32_t(uint32_t(max_y) - uint32_t(iy));
  if (cw < 0 || ch < 0) cw = ch = 0;
  const float fw = float(cw), fh = float(ch);
  StoreGuestF32(device + kViewportCache, x);
  StoreGuestF32(device + kViewportCache + 4, y);
  StoreGuestF32(device + kViewportCache + 8, fw);
  StoreGuestF32(device + kViewportCache + 12, fh);
  StoreGuestF32(device + kViewportCache + 16, min_z);
  StoreGuestF32(device + kViewportCache + 20, max_z);
  StoreGuestU32(device + kViewportCache + 24, 0);
  SetGuestScissorRect(device, device + kScissorCache);
  const float z_scale = max_z - min_z;
  StoreGuestF32(device + 0x2918, z_scale);
  StoreGuestF32(device + 0x291C, min_z);
  const float sx = fw * 0.5f;
  const float ox = sx + x;
  const float sy = fh * 0.5f;
  const float oy = sy + y;
  StoreGuestF32(device + 0x2908, sx);
  StoreGuestF32(device + 0x290C, ox);
  StoreGuestF32(device + 0x2910, -sy);
  StoreGuestF32(device + 0x2914, oy);
  OrGuestU32(device + 0x14, 0x07E00000u);
}

}  // namespace cherry::gpu::hooks

// D3DDevice_SetRenderState_AlphaBlendEnable(D3DDevice*, DWORD Value)
REX_HOOK(D3DDevice_SetRenderState_AlphaBlendEnable, D3DDevice_SetRenderState_AlphaBlendEnable_hook)
// D3DDevice_SetRenderState_SrcBlend(D3DDevice*, DWORD Value)
REX_HOOK(D3DDevice_SetRenderState_SrcBlend, D3DDevice_SetRenderState_SrcBlend_hook)
// D3DDevice_SetRenderState_DestBlend(D3DDevice*, DWORD Value)
REX_HOOK(D3DDevice_SetRenderState_DestBlend, D3DDevice_SetRenderState_DestBlend_hook)
// D3DDevice_SetRenderState_SeparateAlphaBlendEnable(D3DDevice*, DWORD Value)
REX_HOOK(D3DDevice_SetRenderState_SeparateAlphaBlendEnable, D3DDevice_SetRenderState_SeparateAlphaBlendEnable_hook)
// D3DDevice_SetRenderState_SlopeScaleDepthBias(D3DDevice*, DWORD FloatBits)
REX_HOOK(D3DDevice_SetRenderState_SlopeScaleDepthBias, D3DDevice_SetRenderState_SlopeScaleDepthBias_hook)
// D3DDevice_SetRenderState_DepthBias(D3DDevice*, DWORD FloatBits)
REX_HOOK(D3DDevice_SetRenderState_DepthBias, D3DDevice_SetRenderState_DepthBias_hook)
// D3DDevice_SetRenderState_HighPrecisionBlendEnable(D3DDevice*, DWORD Value)
REX_HOOK(D3DDevice_SetRenderState_HighPrecisionBlendEnable, D3DDevice_SetRenderState_HighPrecisionBlendEnable_hook)
// D3DDevice_SetRenderState_HighPrecisionBlendEnable1(D3DDevice*, DWORD Value)
REX_HOOK(D3DDevice_SetRenderState_HighPrecisionBlendEnable1, D3DDevice_SetRenderState_HighPrecisionBlendEnable1_hook)
// D3DDevice_SetSamplerState_MinFilter(D3DDevice*, DWORD Sampler, DWORD Value)
REX_HOOK(D3DDevice_SetSamplerState_MinFilter, D3DDevice_SetSamplerState_MinFilter_hook)
// D3DDevice_SetSamplerState_MagFilter(D3DDevice*, DWORD Sampler, DWORD Value)
REX_HOOK(D3DDevice_SetSamplerState_MagFilter, D3DDevice_SetSamplerState_MagFilter_hook)
// D3DDevice_SetSamplerState_MaxAnisotropy(D3DDevice*, DWORD Sampler, DWORD Value)
REX_HOOK(D3DDevice_SetSamplerState_MaxAnisotropy, D3DDevice_SetSamplerState_MaxAnisotropy_hook)
// D3DDevice_SetSamplerState_MipMapLodBias(D3DDevice*, DWORD Sampler, DWORD FloatBits)
REX_HOOK(D3DDevice_SetSamplerState_MipMapLodBias, D3DDevice_SetSamplerState_MipMapLodBias_hook)
// D3DDevice_SetSamplerState_MaxMipLevel(D3DDevice*, DWORD Sampler, DWORD Value)
REX_HOOK(D3DDevice_SetSamplerState_MaxMipLevel, D3DDevice_SetSamplerState_MaxMipLevel_hook)
// D3DDevice_SetSamplerState_MinMipLevel(D3DDevice*, DWORD Sampler, DWORD Value)
REX_HOOK(D3DDevice_SetSamplerState_MinMipLevel, D3DDevice_SetSamplerState_MinMipLevel_hook)
// D3DDevice_SetScissorRect(D3DDevice*, const RECT*)
REX_HOOK(D3DDevice_SetScissorRect, D3DDevice_SetScissorRect_hook)
// D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*)
REX_HOOK(D3DDevice_SetViewport, D3DDevice_SetViewport_hook)
// D3DDevice_SetGammaRamp(D3DDevice*, const D3DGAMMARAMP*)
REX_HOOK(D3DDevice_SetGammaRamp, D3DDevice_SetGammaRamp_hook)
// D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags, DWORD VertexShaderCount, DWORD PixelShaderCount)
REX_HOOK(D3DDevice_SetShaderGPRAllocation, D3DDevice_SetShaderGPRAllocation_hook)
