#include "gpu/debug/d3d_verify.h"

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "core/guest_call.h"
#include "gpu/guest_memory.h"

REXCVAR_DEFINE_BOOL(cherry_debug_verify_d3d, false, "Lollipop/Debug", "Re-run the game's own XG layout functions after the native ones and log any difference.");

REX_IMPORT(__imp__XGPadMipLevel, OriginalPadMipLevel, u32(u32, u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__XGCalcTextureLayout, OriginalCalcTextureLayout, u32(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__D3DBaseTexture_GetMipLayout, OriginalGetMipLayout, u32(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32));
REX_IMPORT(__imp__XGSetTextureHeaderInternal, OriginalSetTextureHeader, u32(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32));

namespace cherry::gpu::debug {

namespace {

std::atomic<uint32_t> g_checked{0};
std::atomic<uint32_t> g_mismatches{0};

bool Enabled() {
  return REXCVAR_GET(cherry_debug_verify_d3d);
}

// Compares the native words already at `addresses` with what the original then writes there; the original's are left in place.
template <typename Run>
void Compare(const char* name, std::initializer_list<uint32_t> addresses, uint32_t native_result, Run run) {
  uint32_t native[16] = {};
  uint32_t i = 0;
  for (uint32_t a : addresses) native[i++] = LoadGuestU32(a);
  const uint32_t original_result = run();
  bool same = original_result == native_result;
  i = 0;
  for (uint32_t a : addresses) same &= LoadGuestU32(a) == native[i++];
  const uint32_t checked = ++g_checked;
  if (!same && g_mismatches++ < 32) {
    i = 0;
    for (uint32_t a : addresses) {
      const uint32_t original = LoadGuestU32(a);
      if (original != native[i]) REXGPU_WARN("[verify] {}: output {} native {:08X} original {:08X}", name, i, native[i], original);
      ++i;
    }
    if (original_result != native_result) REXGPU_WARN("[verify] {}: result native {:08X} original {:08X}", name, native_result, original_result);
  }
  if ((checked % 2000) == 0) REXGPU_INFO("[verify] {} XG calls checked, {} mismatched", checked, g_mismatches.load());
}

}  // namespace

void VerifyPadMipLevel(const uint32_t in[3], uint32_t width_io, uint32_t height_io, uint32_t depth_io, uint32_t bpp, uint32_t format, uint32_t dimension, uint32_t tiled, uint32_t native_size) {
  if (!Enabled()) return;
  const uint32_t native[3] = {LoadGuestU32(width_io), LoadGuestU32(height_io), LoadGuestU32(depth_io)};
  StoreGuestU32(width_io, in[0]);
  StoreGuestU32(height_io, in[1]);
  StoreGuestU32(depth_io, in[2]);
  const uint32_t original = cherry::core::CallGuest(OriginalPadMipLevel, width_io, height_io, depth_io, bpp, format, dimension, tiled);
  const bool same = original == native_size && LoadGuestU32(width_io) == native[0] && LoadGuestU32(height_io) == native[1] && LoadGuestU32(depth_io) == native[2];
  ++g_checked;
  if (!same && g_mismatches++ < 32) REXGPU_WARN("[verify] XGPadMipLevel({}x{}x{}, bpp {}, format {}, dim {}, tiled {}): native {}x{}x{} {} original {}x{}x{} {}", in[0], in[1], in[2], bpp, format, dimension, tiled, native[0], native[1], native[2], native_size, LoadGuestU32(width_io), LoadGuestU32(height_io), LoadGuestU32(depth_io), original);
}

void VerifyCalcTextureLayout(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format, uint32_t dimension, uint32_t tiled, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t aligned_width_out, uint32_t base_size_out, uint32_t mip_size_out, uint32_t native_levels) {
  if (!Enabled()) return;
  Compare("XGCalcTextureLayout", {aligned_width_out, base_size_out, mip_size_out}, native_levels, [&] { return cherry::core::CallGuest(OriginalCalcTextureLayout, width, height, depth, levels, format, dimension, tiled, packed, border, exp_bias, pitch, aligned_width_out, base_size_out, mip_size_out); });
}

void VerifyGetMipLayout(uint32_t texture, uint32_t face, uint32_t level, uint32_t pitch_out, uint32_t slice_out, uint32_t size_out, uint32_t tail_out, uint32_t width_out, uint32_t height_out, uint32_t depth_out, uint32_t address_out) {
  if (!Enabled()) return;
  // Its r3 is whatever the last helper left there, so only the outputs are compared.
  Compare("D3DBaseTexture_GetMipLayout", {pitch_out, slice_out, size_out, tail_out, width_out, height_out, depth_out, address_out}, 0, [&] {
    cherry::core::CallGuest(OriginalGetMipLayout, texture, face, level, pitch_out, slice_out, size_out, tail_out, width_out, height_out, depth_out, address_out);
    return 0u;
  });
}

void VerifySetTextureHeader(uint32_t type, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t texture, uint32_t base_size_out, uint32_t mip_size_out) {
  if (!Enabled()) return;
  Compare("XGSetTextureHeaderInternal", {base_size_out, mip_size_out, texture, texture + 4, texture + 8, texture + 0x14, texture + 0x18, texture + 0x1C, texture + 0x20, texture + 0x24, texture + 0x28, texture + 0x2C, texture + 0x30}, 0, [&] {
    cherry::core::CallGuest(OriginalSetTextureHeader, type, width, height, depth, levels, usage, format, packed, border, exp_bias, pitch, texture, base_size_out, mip_size_out);
    return 0u;
  });
}

}  // namespace cherry::gpu::debug
