#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>

#include "re_cherry_init.h"

#include <rex/hook.h>

#include "gpu/debug/d3d_verify.h"
#include "core/guest_call.h"
#include "gpu/guest_memory.h"
#include "gpu/textures/texture_layout.h"

// The D3D and XG texture and surface functions UE3 calls, written natively: headers, layout arithmetic, locks and child surfaces. Memory still comes from XMemAlloc and EDRAM tiles from the game's own allocator state.

REX_IMPORT(__imp__sub_82BF5230, GuestXMemAlloc, u32(u32, u32));
REX_IMPORT(__imp__sub_82BF5368, GuestXMemFree, u32(u32, u32));
REX_IMPORT(__imp__RtlInitializeCriticalSection, GuestInitializeCriticalSection, u32(u32));
REX_IMPORT(__imp__RtlEnterCriticalSection, GuestEnterCriticalSection, u32(u32));
REX_IMPORT(__imp__RtlLeaveCriticalSection, GuestLeaveCriticalSection, u32(u32));

namespace {

using namespace cherry::gpu;

constexpr uint32_t kHiZClaimed = 0x83340874;
constexpr uint32_t kEdramAllocator = 0x83340878;
constexpr uint32_t kHeapAttributes = 0x64800000;
constexpr uint32_t kEmptyFlushRange = 0xFFFF0000;

uint32_t Cntlzw(uint32_t v) { return uint32_t(std::countl_zero(v)); }
uint32_t Slw(uint32_t v, uint32_t n) { return (n & 0x20) ? 0 : v << (n & 0x1F); }
uint32_t Srw(uint32_t v, uint32_t n) { return (n & 0x20) ? 0 : v >> (n & 0x1F); }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
uint32_t Page(uint32_t v) { return (v + 0xFFF) & ~0xFFFu; }
uint32_t AtLeastOne(uint32_t v) { return v > 1 ? v : 1; }

uint32_t AtomicAddGuest(uint32_t address, uint32_t delta) {
  auto& word = *reinterpret_cast<std::atomic<uint32_t>*>(cherry::gpu::GuestBase() + address);
  uint32_t stored = word.load();
  for (;;) {
    const uint32_t next = std::byteswap(stored) + delta;
    if (word.compare_exchange_weak(stored, std::byteswap(next))) return next;
  }
}

// D3DResource_GetType (0x829DFDE0).
uint32_t ResourceType(uint32_t resource) {
  const uint32_t common = LoadGuestU32(resource), type = common & 0xF;
  if (type == 3) {
    const uint32_t dimension = (LoadGuestU32(resource + 0x30) >> 9) & 3;
    if (dimension == 3) return 0x12;
    if (dimension == 2) return 0x11;
    if (dimension == 0) return 0x14;
    return (LoadGuestU32(resource + 0x20) & 0x400) ? 0x13 : 3;
  }
  if (type == 4 && (common & 0x40000000) && (LoadGuestU32(LoadGuestU32(resource + 0x18) + 0x30) & 0x600) == 0x400) return 0x10;
  return type;
}

// D3DResource_AddRef (0x829DFC70).
uint32_t AddRef(uint32_t resource) {
  const uint32_t count = AtomicAddGuest(resource + 4, 1);
  if (count == 1) {
    const uint32_t common = LoadGuestU32(resource);
    if ((common & 0xF) == 4 && (common & 0x40000000)) AddRef(LoadGuestU32(resource + 0x18));
  }
  return count;
}

uint32_t MipLevelCount(uint32_t w, uint32_t h, uint32_t d, uint32_t border) {
  const uint32_t b2 = border * 2;
  const int32_t dd = int32_t(d - b2);
  const uint32_t dm = dd > 1 ? uint32_t(dd) : 1;
  const uint32_t hm = (h - b2) > dm ? h - b2 : dm;
  return 32 - Cntlzw((w - b2) > hm ? w - b2 : hm);
}

struct TextureSizes {
  uint32_t levels, aligned_width, base_size, mip_size;
};

TextureSizes CalcTextureLayout(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format, uint32_t dimension, uint32_t tiled, uint32_t packed, uint32_t border, uint32_t pitch) {
  const bool array = dimension == 1 && depth > 1;
  const bool layered = array || dimension == 3;
  uint32_t depth_cur = layered ? 1 : depth;
  const uint32_t b2 = border * 2, w_in = width - b2, h_in = height - b2, bpp = FormatBitsPerTexel(format);
  uint32_t log_w = border - Cntlzw(w_in - 1) + 32, log_h = border - Cntlzw(h_in - 1) + 32;
  uint32_t d_in = 1, log_d = 0, row_pitch = dimension == 0 ? 0 : pitch;
  if (dimension == 2) {
    d_in = depth_cur - b2;
    log_d = border - Cntlzw(d_in - 1) + 32;
  }
  uint32_t pw = Slw(1, log_w), ph = Slw(1, log_h), pd = Slw(1, log_d);
  if ((pw > 16 && ph > 16) || !packed) {
    pw = row_pitch ? (row_pitch << 3) / bpp : width;
    ph = height;
    pd = depth_cur;
  }
  PadMipLevel(pw, ph, pd, bpp, format, dimension, tiled);
  const uint32_t aligned_width = pw;
  if (!row_pitch) row_pitch = (aligned_width * bpp) >> 3;
  uint32_t base_size, mip_size = 0;
  if (dimension == 2) {
    base_size = Page(row_pitch * pd * ph);
  } else if (tiled || layered) {
    base_size = Page(row_pitch * ph) * pd;
  } else if (dimension == 1 && levels == 1 && !packed && !border) {
    uint32_t bw, bh;
    FormatBlockSize(format, bw, bh);
    return {levels, aligned_width, AlignUp(height, bh) * row_pitch, 0};
  } else {
    base_size = row_pitch * pd * ph;
  }
  if (layered) base_size *= depth;
  if (levels > 1 || levels == 0) {
    const uint32_t m1 = h_in > d_in ? h_in : d_in;
    if (!levels) levels = 32 - Cntlzw(w_in > m1 ? w_in : m1);
    for (uint32_t n = levels - 1; n; --n) {
      if (packed && (log_w <= 4 || log_h <= 4)) break;
      if (log_w) --log_w;
      if (log_h) --log_h;
      if (depth_cur > 1) depth_cur >>= 1;
      uint32_t mw = Slw(1, log_w), mh = Slw(1, log_h), md = depth_cur;
      mip_size += PadMipLevel(mw, mh, md, bpp, format, dimension, tiled);
    }
    if (layered) mip_size *= AlignUp(depth, array ? 4 : 1);
  }
  return {levels, aligned_width, base_size, mip_size};
}

void TextureDims(uint32_t texture, uint32_t& w, uint32_t& h, uint32_t& d) {
  const uint32_t size = LoadGuestU32(texture + 0x24), add = ((LoadGuestU32(texture + 0x28) >> 30) & 2) + 1;
  switch ((LoadGuestU32(texture + 0x30) >> 9) & 3) {
    case 0:
      w = (size & 0xFFFFFF) + add;
      h = 1;
      d = 1;
      break;
    case 2:
      w = (size & 0x7FF) + add;
      h = ((size >> 11) & 0x7FF) + add;
      d = (size >> 22) + add;
      break;
    default:
      w = (size & 0x1FFF) + add;
      h = ((size >> 13) & 0x1FFF) + add;
      d = 1;
      break;
  }
}

struct MipLayout {
  uint32_t pitch, slice, size, tail, width, height, depth, address, ret;
};

MipLayout GetMipLayout(uint32_t texture, uint32_t face, uint32_t level) {
  const uint32_t d0 = LoadGuestU32(texture + 0x1C), d1 = LoadGuestU32(texture + 0x20), d3 = LoadGuestU32(texture + 0x28), d5 = LoadGuestU32(texture + 0x30);
  const uint32_t border = d3 >> 31, b2 = border * 2, bpp = FormatBitsPerTexel(d1), tiled = d0 >> 31, dimension = (d5 >> 9) & 3, format = d1 & 0x3F;
  MipLayout m{};
  TextureDims(texture, m.width, m.height, m.depth);
  const uint32_t w_in = m.width - b2, h_in = m.height - b2;
  const uint32_t pw = Slw(1, border - Cntlzw(w_in - 1) + 32);
  uint32_t rows = Slw(1, border - Cntlzw(h_in - 1) + 32);
  uint32_t slices = dimension == 2 ? Slw(1, border - Cntlzw(m.depth - b2 - 1) + 32) : 1;
  const bool packed = !(pw > 16 && rows > 16) && (d5 & 0x800);
  uint32_t address = (level == 0 || packed) ? (d1 & ~0xFFFu) : (d5 & ~0xFFFu);
  if (level == 0 && !packed) {
    uint32_t w = m.width, h = m.height, d = m.depth;
    m.ret = PadMipLevel(w, h, d, bpp, format, dimension, tiled);
    if (!tiled && !(d1 & 0x400) && !(d5 & 0x800) && dimension == 1 && !(LoadGuestU32(texture + 0x2C) & 0x3C0) && !border) {
      uint32_t bw, bh;
      FormatBlockSize(format, bw, bh);
      m.ret = format;
      rows = AlignUp(m.height, bh);
    } else {
      rows = h;
    }
    slices = d;
    m.pitch = dimension == 0 ? (w * bpp) >> 3 : ((((d0 >> 22) & 0x1FF) * bpp) << 2) & 0x1FFFFFFC;
  } else {
    m.width = AtLeastOne(Srw(w_in, level)) + b2;
    m.height = AtLeastOne(Srw(h_in, level)) + b2;
    m.depth = dimension == 2 ? AtLeastOne(Srw(m.depth - b2, level)) + b2 : 1;
    m.ret = ResourceType(texture);
    const uint32_t align = Slw(1, (d1 >> 9) & 2);
    const uint32_t layers = (m.ret == 0x12 || m.ret == 0x13) ? ((LoadGuestU32(texture + 0x24) >> 26) + align) & ~(align - 1) : 1;
    uint32_t log_w = 31 - Cntlzw(pw), log_h = 31 - Cntlzw(rows), log_d = 31 - Cntlzw(slices);
    int32_t i = int32_t(level) - 1;
    if (packed) {
      i = int32_t(level);
      ++log_w;
      ++log_h;
      ++log_d;
    }
    for (; i >= 0; --i) {
      if (log_w) --log_w;
      if (log_h) --log_h;
      if (log_d) --log_d;
      const uint32_t lw = Slw(1, log_w), lh = Slw(1, log_h), ld = Slw(1, log_d);
      uint32_t w = lw, h = lh, d = ld;
      m.ret = PadMipLevel(w, h, d, bpp, format, dimension, tiled);
      m.pitch = (w * bpp) >> 3;
      rows = h;
      slices = d;
      if (!(lw > 16 && lh > 16) && (d5 & 0x800)) {
        uint32_t x, y, z;
        m.ret = MipTailOffset(uint32_t(i), lw, lh, ld, m.pitch, m.pitch * rows, format, x, y, z);
        address += m.ret;
        m.tail = m.ret;
        break;
      }
      if (i > 0) address += (dimension == 2 ? Page(m.pitch * slices * rows) : Page(m.pitch * rows) * slices) * layers;
    }
  }
  const uint32_t row_bytes = m.pitch * rows, slice_bytes = Page(row_bytes);
  m.address = slice_bytes * face + address;
  if ((d5 & 0x600) == 0x400) {
    m.slice = row_bytes;
    m.size = Page(row_bytes * slices);
  } else {
    m.slice = slice_bytes;
    m.size = slice_bytes * slices;
  }
  return m;
}

// The lock core (D3D_LockResource) for textures: the lock count and the CPU flush range; no fence wait and no GPU cache invalidation.
uint32_t LockResource(uint32_t resource, uint32_t level, uint32_t base_address, uint32_t mip_address, uint32_t address, uint32_t size, uint32_t flags) {
  AtomicAddGuest(resource, 0x100);
  if ((flags & 0x10) || (LoadGuestU32(resource) & 0x200000)) {
    if (!(flags & 1)) {
      const bool mip = level && mip_address;
      const uint32_t origin = (mip ? mip_address : base_address) & ~0x7Fu, slot = resource + (mip ? 0x18 : 0x14);
      const uint32_t offset = address - origin, first = offset >> 7, last = (offset + size + 0x7F) >> 7;
      const uint32_t range = LoadGuestU32(slot);
      uint32_t lo = range >> 16, hi = range & 0xFFFF;
      if (lo & 0x8000) lo = (lo & 0x7FFF) << 4;
      if (hi & 0x8000) hi = (hi & 0x7FFF) << 4;
      if (last > hi) hi = last;
      if (first < lo) lo = first;
      if (lo >= 0x7FFF) lo = (lo + 0x80000) >> 4;
      if (hi >= 0x7FFF) hi = (hi + 0x8000F) >> 4;
      StoreGuestU32(slot, (lo << 16) | hi);
    }
    if (flags & 0x10) return (((address >> 20) + 0x200) & 0x1000) + (address & 0x1FFFFFFF) - 0x40000000;
  }
  return address;
}

void UnlockResource(uint32_t resource, uint32_t mip_address) {
  if ((LoadGuestU32(resource) & 0xF00) == 0x100) {
    if (LoadGuestU32(resource + 0x14) != kEmptyFlushRange) StoreGuestU32(resource + 0x14, kEmptyFlushRange);
    if (mip_address && LoadGuestU32(resource + 0x18) != kEmptyFlushRange) StoreGuestU32(resource + 0x18, kEmptyFlushRange);
  }
  AtomicAddGuest(resource, 0xFFFFFF00);
}

uint32_t LockLevel(uint32_t texture, uint32_t face, uint32_t level, uint32_t flags, uint32_t& address, uint32_t& pitch, uint32_t& slice, uint32_t& tail) {
  const MipLayout m = GetMipLayout(texture, face, level);
  const uint32_t d1 = LoadGuestU32(texture + 0x20);
  uint32_t bw, bh;
  FormatBlockSize(d1 & 0x3F, bw, bh);
  pitch = m.pitch * bh;
  slice = m.slice;
  tail = m.tail;
  const uint32_t result = LockResource(texture, level, d1 & ~0xFFFu, LoadGuestU32(texture + 0x30) & ~0xFFFu, m.address - m.tail, m.size, flags);
  address = m.tail + result;
  return result;
}

uint32_t LockRect(uint32_t texture, uint32_t face, uint32_t level, uint32_t locked_rect, uint32_t rect, uint32_t flags) {
  uint32_t address, pitch, slice, tail;
  const uint32_t result = LockLevel(texture, face, level, flags, address, pitch, slice, tail);
  uint32_t bits = address;
  if (rect) bits += ((FormatBitsPerTexel(LoadGuestU32(texture + 0x20)) * LoadGuestU32(rect)) >> 3) + LoadGuestU32(rect + 4) * pitch;
  StoreGuestU32(locked_rect, pitch);
  StoreGuestU32(locked_rect + 4, bits);
  return result;
}

uint32_t LockBox(uint32_t texture, uint32_t level, uint32_t locked_box, uint32_t box, uint32_t flags) {
  uint32_t address, pitch, slice, tail;
  uint32_t result = LockLevel(texture, 0, level, flags, address, pitch, slice, tail);
  uint32_t bits = address;
  if (box) {
    result = address;
    bits += ((FormatBitsPerTexel(LoadGuestU32(texture + 0x20)) * LoadGuestU32(box)) >> 3) + LoadGuestU32(box + 4) * pitch + LoadGuestU32(box + 0x10) * slice;
  }
  StoreGuestU32(locked_box, pitch);
  StoreGuestU32(locked_box + 4, slice);
  StoreGuestU32(locked_box + 8, bits);
  return result;
}

uint32_t SetTextureHeader(uint32_t type, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t texture, uint32_t& base_size, uint32_t& mip_size) {
  const uint32_t b1 = border * 2 + 1;
  uint32_t dimension = 1, level_depth = 1;
  switch (type) {
    case 0x11:
      dimension = 2;
      level_depth = depth;
      break;
    case 0x12: dimension = 3; break;
    case 0x14:
      dimension = 0;
      packed = 0;
      break;
    default: break;
  }
  if (!levels) levels = MipLevelCount(width, height, level_depth, border);
  const uint32_t packed_flag = packed == 2 ? (levels > 1 ? 1 : 0) : packed;
  const TextureSizes sizes = CalcTextureLayout(width, height, depth, levels, format & 0x3F, dimension, (format >> 8) & 1, packed_flag, border, pitch);
  uint32_t common = (usage & 4) ? 0x300003 : 0x100003;
  if (usage & 0x200) common |= 0x400000;
  StoreGuestU32(texture, common);
  StoreGuestU32(texture + 4, 1);
  StoreGuestU32(texture + 0x14, kEmptyFlushRange);
  StoreGuestU32(texture + 0x18, kEmptyFlushRange);
  StoreGuestU32(texture + 8, 0);
  const uint32_t d5 = (LoadGuestU32(texture + 0x30) & ~0x600u) | ((dimension & 3) << 9);
  const uint32_t d2 = LoadGuestU32(texture + 0x24);
  switch (dimension) {
    case 0: StoreGuestU32(texture + 0x24, (d2 & 0xFF000000) | ((width - b1) & 0xFFFFFF)); break;
    case 1: StoreGuestU32(texture + 0x24, ((depth - 1) << 26) | (((height - b1) & 0x1FFF) << 13) | ((width - b1) & 0x1FFF)); break;
    case 2: StoreGuestU32(texture + 0x24, ((depth - b1) << 22) | (((height - b1) & 0x7FF) << 11) | ((width - b1) & 0x7FF)); break;
    default: StoreGuestU32(texture + 0x24, (((depth - 1) & 0x3F) << 26) | (((height - b1) & 0x1FFF) << 13) | ((width - b1) & 0x1FFF)); break;
  }
  const uint32_t d0 = LoadGuestU32(texture + 0x1C), d1 = LoadGuestU32(texture + 0x20), d3 = LoadGuestU32(texture + 0x28), d4 = LoadGuestU32(texture + 0x2C);
  StoreGuestU32(texture + 0x1C, (d0 & 0x003FFC00) | (((sizes.aligned_width >> 5) & 0x1FF) << 22) | (((format >> 8) & 1) << 31) | (((format >> 9) & 0xFF) << 2) | 2);
  StoreGuestU32(texture + 0x2C, (d4 & ~0x3C0u) | (((levels - 1) & 0xF) << 6));
  StoreGuestU32(texture + 0x20, (d1 & 0xFFFFFB00) | (format & 0xFF) | (type == 0x13 ? 0x400 : 0));
  StoreGuestU32(texture + 0x30, (d5 & ~0x800u) | ((packed_flag & 1) << 11));
  StoreGuestU32(texture + 0x28, ((format >> 17) & 0x1FFF) | ((exp_bias & 0x3F) << 13) | ((border & 1) << 31) | (d3 & 0x7FF80000));
  base_size = sizes.base_size;
  mip_size = sizes.mip_size;
  return (format >> 9) & 0xFF;
}

// The D3DFORMAT a texture header encodes.
uint32_t TextureFormat(uint32_t texture) {
  const uint32_t d0 = LoadGuestU32(texture + 0x1C), d1 = LoadGuestU32(texture + 0x20), d3 = LoadGuestU32(texture + 0x28);
  return ((d3 & 0x1FFF) << 17) | (((d0 >> 2) & 0xFF) << 9) | ((d0 >> 31) << 8) | (((d1 >> 6) & 3) << 6) | (d1 & 0x3F);
}

void LevelDims(uint32_t texture, uint32_t level, uint32_t& w, uint32_t& h, uint32_t& d) {
  TextureDims(texture, w, h, d);
  if (ResourceType(texture) == 4 || level == 0) return;
  const uint32_t b2 = (LoadGuestU32(texture + 0x28) >> 31) * 2;
  w = AtLeastOne(Srw(w - b2, level)) + b2;
  h = AtLeastOne(Srw(h - b2, level)) + b2;
  d = ((LoadGuestU32(texture + 0x30) >> 9) & 3) == 2 ? AtLeastOne(Srw(d - b2, level)) + b2 : 1;
}

void TextureLevelDesc(uint32_t texture, uint32_t level, uint32_t desc) {
  uint32_t w, h, d;
  LevelDims(texture, level, w, h, d);
  StoreGuestU32(desc, TextureFormat(texture));
  StoreGuestU32(desc + 4, ResourceType(texture));
  StoreGuestU32(desc + 8, 0);
  StoreGuestU32(desc + 0xC, 0);
  StoreGuestU32(desc + 0x14, 0);
  StoreGuestU32(desc + 0x18, w);
  StoreGuestU32(desc + 0x1C, h);
  StoreGuestU32(desc + 0x10, 0);
}

void VolumeLevelDesc(uint32_t texture, uint32_t level, uint32_t desc) {
  uint32_t w, h, d;
  LevelDims(texture, level, w, h, d);
  StoreGuestU32(desc, TextureFormat(texture));
  StoreGuestU32(desc + 4, ResourceType(texture));
  StoreGuestU32(desc + 8, 0);
  StoreGuestU32(desc + 0xC, 0);
  StoreGuestU32(desc + 0x10, w);
  StoreGuestU32(desc + 0x14, h);
  StoreGuestU32(desc + 0x18, d);
}

// EDRAM tiles of 5120 bytes a surface of this size and format needs.
uint32_t SurfaceTiles(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample) {
  uint32_t w = width, h = height;
  const uint32_t fmt = format & 0x3F;
  if (int32_t(multisample) >= 1) h = height * 2;
  if (int32_t(multisample) == 2) w = width * 2;
  const uint32_t bytes = (fmt == 0x15 || fmt == 0x20 || fmt == 0x25) ? 8 : 4;
  return ((h + 15) & ~15u) * (((w + 79) / 80) * 80) * bytes / 0x1400;
}

uint32_t InitSurfaceHeader(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, uint32_t params, uint32_t surface) {
  uint32_t fmt = format & 0x3F;
  const bool depth = fmt == 0x16 || fmt == 0x17;
  if (fmt == 0x36) fmt = 7;
  StoreGuestU32(surface + 4, 1);
  StoreGuestU32(surface, 4);
  StoreGuestU32(surface + 0x14, kEmptyFlushRange);
  int32_t hiz_base = -1;
  const uint32_t tiles = SurfaceTiles(width, height, format, multisample);
  uint32_t base = 0, exp_bias = 0;
  if (params) {
    base = LoadGuestU32(params);
    hiz_base = int32_t(LoadGuestU32(params + 4));
    exp_bias = LoadGuestU32(params + 8);
  } else if (depth) {
    hiz_base = LoadGuestU32(kHiZClaimed) ? -1 : 0;
  }
  const bool wide = multisample == 0 || multisample == 1;
  const uint32_t pitch = wide ? 80 * ((width + 79) / 80) : 40 * ((width + 39) / 40);
  const uint32_t aligned = wide ? (width + 31) & ~31u : (width + 15) & ~15u;
  const uint32_t size_word = LoadGuestU32(surface + 0x24);
  StoreGuestU32(surface + 0x28, format);
  StoreGuestU32(surface + 0x24, ((width - 1) << 18) | (((height - 1) & 0x7FFF) << 3) | (size_word & 7));
  StoreGuestU32(surface + 0x18, (((aligned << 2) | (multisample & 3)) << 16) | (pitch & 0x3FFF));
  uint32_t flags = FormatFlags(fmt);
  if (depth) {
    StoreGuestU32(surface + 0x1C, ((flags & 1) << 16) | (base & 0xFFF));
    if (hiz_base == -1) {
      StoreGuestU32(surface + 0x20, 0);
    } else {
      const uint32_t func = (params && LoadGuestU32(params + 0xC)) ? LoadGuestU32(params + 0xC) - 1 : (fmt != 0x16 ? 1 : 0);
      StoreGuestU32(surface + 0x20, (((func & 1) | (uint32_t(hiz_base) << 13)) << 4) | 1);
    }
  } else {
    if (!flags && !(format & 0x20000) && (format & 0x600) == 0x600 && (format & 0x1800) == 0x1800 && (format & 0x6000) == 0x6000 && !(format & 0x18000)) flags = 1;
    StoreGuestU32(surface + 0x1C, ((((flags & ~0x3F0u) | ((exp_bias & 0x3F) << 4)) & 0x3FF) << 16) | (base & 0xFFF));
  }
  StoreGuestU32(surface + 0x2C, tiles * 0x1400);
  return tiles;
}

// The game's EDRAM tile bitmap (sub_829F48F0): one bit per tile at allocator+0x20.
void EdramMark(uint32_t allocator, uint32_t start, uint32_t end, bool used) {
  uint32_t p = allocator + 0x20 + 4 * (start >> 5);
  const uint32_t e = allocator + 0x20 + 4 * (end >> 5), head = 0xFFFFFFFFu >> (start & 31), tail = 0xFFFFFFFFu >> (end & 31);
  if (p == e) {
    const uint32_t m = head & ~tail;
    StoreGuestU32(p, used ? (LoadGuestU32(p) | m) : (LoadGuestU32(p) & ~m));
    return;
  }
  StoreGuestU32(p, used ? (LoadGuestU32(p) | head) : (LoadGuestU32(p) & ~head));
  for (p += 4; p < e; p += 4) StoreGuestU32(p, used ? 0xFFFFFFFF : 0);
  StoreGuestU32(e, used ? (LoadGuestU32(e) | ~tail) : (LoadGuestU32(e) & tail));
}

uint32_t EdramAllocator() {
  if (const uint32_t allocator = LoadGuestU32(kEdramAllocator)) return allocator;
  const uint32_t allocator = cherry::core::CallGuest(GuestXMemAlloc, 0x324, kHeapAttributes);
  if (!allocator) return 0;
  cherry::core::CallGuest(GuestInitializeCriticalSection, allocator);
  StoreGuestU32(allocator + 0x1C, 0x1800);
  StoreGuestU32(allocator + 0x320, 0x55555555);
  auto& slot = *reinterpret_cast<std::atomic<uint32_t>*>(cherry::gpu::GuestBase() + kEdramAllocator);
  uint32_t expected = 0;
  if (slot.compare_exchange_strong(expected, std::byteswap(allocator))) return allocator;
  cherry::core::CallGuest(GuestXMemFree, allocator, kHeapAttributes);
  return LoadGuestU32(kEdramAllocator);
}

// Best-fit run of free tiles (sub_829F4A30).
bool EdramAlloc(uint32_t count, uint32_t& out) {
  const uint32_t allocator = EdramAllocator();
  if (!allocator) return false;
  cherry::core::CallGuest(GuestEnterCriticalSection, allocator);
  uint32_t found = 0xFFFFFFFF;
  const uint32_t free_tiles = LoadGuestU32(allocator + 0x1C);
  if (count <= free_tiles) {
    const uint32_t words = allocator + 0x20, end = allocator + 0x320;
    uint32_t cursor = 0, best = 0, best_len = 0xFFFFFFFF;
    for (;;) {
      uint32_t p = words + ((cursor >> 3) & 0x1FFFFFFC);
      if (p == end) break;
      uint32_t word = LoadGuestU32(p);
      uint32_t lead = Cntlzw((0xFFFFFFFFu >> (cursor & 31)) & ~word);
      if (lead == 32) {
        do {
          p += 4;
          word = LoadGuestU32(p);
        } while (word == 0xFFFFFFFF);
        if (p == end) break;
        lead = Cntlzw(~word);
      }
      uint32_t q = p, stop = Cntlzw((0xFFFFFFFFu >> lead) & word);
      if (stop == 32) {
        q = p + 4;
        uint32_t next = LoadGuestU32(q);
        while (!next) {
          q += 4;
          next = LoadGuestU32(q);
        }
        stop = q == end ? 0 : Cntlzw(next);
      }
      const uint32_t start = ((p - words) >> 2) * 32 + lead, length = ((q - words) >> 2) * 32 - start + stop;
      if (!length) break;
      if (length >= count && length < best_len) {
        best = start;
        best_len = length;
      }
      cursor = start + length;
    }
    if (best_len != 0xFFFFFFFF) {
      StoreGuestU32(allocator + 0x1C, free_tiles - count);
      EdramMark(allocator, best, best + count, true);
      found = best;
    }
  }
  cherry::core::CallGuest(GuestLeaveCriticalSection, allocator);
  out = found;
  return found != 0xFFFFFFFF;
}

// A surface or volume that views one level (and face) of its parent, which it keeps a reference to.
uint32_t NewChild(uint32_t size, uint32_t parent, uint32_t info_mask, uint32_t info_bits) {
  const uint32_t child = cherry::core::CallGuest(GuestXMemAlloc, size, kHeapAttributes);
  if (!child) return 0;
  const uint32_t info = LoadGuestU32(child + 0x1C);
  StoreGuestU32(child + 0x18, parent);
  StoreGuestU32(child, 0x40100004 | (LoadGuestU32(parent) & 0x200000));
  StoreGuestU32(child + 4, 1);
  StoreGuestU32(child + 0x14, kEmptyFlushRange);
  StoreGuestU32(child + 0x1C, (info & info_mask) | info_bits);
  AddRef(parent);
  return child;
}

void D3DDevice_SetBlendState_hook(uint32_t device, uint32_t index, uint32_t state) {
  const uint32_t slot = index ? index + 7 : 0;
  StoreGuestU32(device + 4 * (slot + 0xA4E), state);
  const uint32_t key = slot + 0x20135, shift = key & 0xFF;
  const uint64_t bit = (shift & 0x40) ? 0 : (0x8000000000000000ull >> (shift & 0x3F));
  cherry::gpu::OrGuestU64(device + 8 * uint32_t(int32_t(key) >> 16), bit);
  cherry::gpu::OrGuestU32(device + 0x14, 0x20000);
}

uint32_t XGGetBlockDimensions_hook(uint32_t format, uint32_t width_out, uint32_t height_out) {
  uint32_t bw, bh;
  FormatBlockSize(format, bw, bh);
  StoreGuestU32(width_out, bw);
  StoreGuestU32(height_out, bh);
  return format;
}

uint32_t XGPadMipLevel_hook(uint32_t width_io, uint32_t height_io, uint32_t depth_io, uint32_t bpp, uint32_t format, uint32_t dimension, uint32_t tiled) {
  const uint32_t in[3] = {LoadGuestU32(width_io), LoadGuestU32(height_io), LoadGuestU32(depth_io)};
  uint32_t w = in[0], h = in[1], d = in[2];
  const uint32_t size = PadMipLevel(w, h, d, bpp, format, dimension, tiled);
  StoreGuestU32(width_io, w);
  StoreGuestU32(height_io, h);
  StoreGuestU32(depth_io, d);
  cherry::gpu::debug::VerifyPadMipLevel(in, width_io, height_io, depth_io, bpp, format, dimension, tiled, size);
  return size;
}

uint32_t XGCalcTextureLayout_hook(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t format, uint32_t dimension, uint32_t tiled, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t aligned_width_out, uint32_t base_size_out, uint32_t mip_size_out) {
  const TextureSizes s = CalcTextureLayout(width, height, depth, levels, format, dimension, tiled, packed, border, pitch);
  StoreGuestU32(aligned_width_out, s.aligned_width);
  StoreGuestU32(base_size_out, s.base_size);
  StoreGuestU32(mip_size_out, s.mip_size);
  cherry::gpu::debug::VerifyCalcTextureLayout(width, height, depth, levels, format, dimension, tiled, packed, border, exp_bias, pitch, aligned_width_out, base_size_out, mip_size_out, s.levels);
  return s.levels;
}

uint32_t D3DBaseTexture_GetDimensions_hook(uint32_t texture, uint32_t width_out, uint32_t height_out, uint32_t depth_out) {
  uint32_t w, h, d;
  TextureDims(texture, w, h, d);
  StoreGuestU32(width_out, w);
  StoreGuestU32(height_out, h);
  StoreGuestU32(depth_out, d);
  return texture;
}

uint32_t XGGetFetchConstantDimensions_hook(uint32_t fetch, uint32_t width_out, uint32_t height_out, uint32_t depth_out) {
  const uint32_t size = LoadGuestU32(fetch + 8), add = ((LoadGuestU32(fetch + 0xC) >> 30) & 2) + 1;
  switch ((LoadGuestU32(fetch + 0x14) >> 9) & 3) {
    case 0:
      StoreGuestU32(width_out, (size & 0xFFFFFF) + add);
      StoreGuestU32(height_out, 1);
      StoreGuestU32(depth_out, 1);
      break;
    case 2:
      StoreGuestU32(width_out, (size & 0x7FF) + add);
      StoreGuestU32(height_out, ((size >> 11) & 0x7FF) + add);
      StoreGuestU32(depth_out, (size >> 22) + add);
      break;
    default:
      StoreGuestU32(width_out, (size & 0x1FFF) + add);
      StoreGuestU32(height_out, ((size >> 13) & 0x1FFF) + add);
      StoreGuestU32(depth_out, (size >> 26) + 1);
      break;
  }
  return fetch;
}

uint32_t XGGetMipTailLevelOffset_hook(uint32_t level, uint32_t width, uint32_t height, uint32_t depth, uint32_t row_pitch, uint32_t slice_pitch, uint32_t format, uint32_t x_out, uint32_t y_out, uint32_t z_out) {
  uint32_t x, y, z;
  const uint32_t offset = MipTailOffset(level, width, height, depth, row_pitch, slice_pitch, format, x, y, z);
  StoreGuestU32(x_out, x);
  StoreGuestU32(y_out, y);
  StoreGuestU32(z_out, z);
  return offset;
}

uint32_t D3DBaseTexture_GetMipLayout_hook(uint32_t texture, uint32_t face, uint32_t level, uint32_t pitch_out, uint32_t slice_out, uint32_t size_out, uint32_t tail_out, uint32_t width_out, uint32_t height_out, uint32_t depth_out, uint32_t address_out) {
  const MipLayout m = GetMipLayout(texture, face, level);
  StoreGuestU32(address_out, m.address);
  StoreGuestU32(pitch_out, m.pitch);
  StoreGuestU32(slice_out, m.slice);
  StoreGuestU32(size_out, m.size);
  StoreGuestU32(width_out, m.width);
  StoreGuestU32(height_out, m.height);
  StoreGuestU32(depth_out, m.depth);
  StoreGuestU32(tail_out, m.tail);
  cherry::gpu::debug::VerifyGetMipLayout(texture, face, level, pitch_out, slice_out, size_out, tail_out, width_out, height_out, depth_out, address_out);
  return m.ret;
}

uint32_t D3DBaseTexture_LockLevel_hook(uint32_t texture, uint32_t face, uint32_t level, uint32_t flags, uint32_t address_out, uint32_t pitch_out, uint32_t slice_out, uint32_t tail_out) {
  uint32_t address, pitch, slice, tail;
  const uint32_t result = LockLevel(texture, face, level, flags, address, pitch, slice, tail);
  StoreGuestU32(pitch_out, pitch);
  StoreGuestU32(slice_out, slice);
  StoreGuestU32(tail_out, tail);
  StoreGuestU32(address_out, address);
  return result;
}

uint32_t D3DBaseTexture_LockRect_hook(uint32_t texture, uint32_t face, uint32_t level, uint32_t locked_rect, uint32_t rect, uint32_t flags) {
  return LockRect(texture, face, level, locked_rect, rect, flags);
}

uint32_t D3DVolumeTexture_LockBox_hook(uint32_t texture, uint32_t level, uint32_t locked_box, uint32_t box, uint32_t flags) {
  return LockBox(texture, level, locked_box, box, flags);
}

uint32_t XGSetTextureHeaderInternal_hook(uint32_t type, uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t packed, uint32_t border, uint32_t exp_bias, uint32_t pitch, uint32_t texture, uint32_t base_size_out, uint32_t mip_size_out) {
  uint32_t base_size, mip_size;
  const uint32_t result = SetTextureHeader(type, width, height, depth, levels, usage, format, packed, border, exp_bias, pitch, texture, base_size, mip_size);
  StoreGuestU32(base_size_out, base_size);
  StoreGuestU32(mip_size_out, mip_size);
  cherry::gpu::debug::VerifySetTextureHeader(type, width, height, depth, levels, usage, format, packed, border, exp_bias, pitch, texture, base_size_out, mip_size_out);
  return result;
}

uint32_t D3DBaseTexture_GetLevelCount_hook(uint32_t texture) {
  return ((LoadGuestU32(texture + 0x2C) >> 6) & 0xF) + 1;
}

uint32_t D3DBaseTexture_LockTail_hook(uint32_t texture, uint32_t face, uint32_t locked_tail, uint32_t flags) {
  uint32_t w, h, d;
  TextureDims(texture, w, h, d);
  const uint32_t border = LoadGuestU32(texture + 0x28) >> 31;
  const int32_t lw = int32_t(border - Cntlzw(w - 2 * border - 1) + 32), lh = int32_t(border - Cntlzw(h - 2 * border - 1) + 32);
  const int32_t tail_level = std::min(lw, lh) - 4;
  uint32_t address, pitch, slice, tail;
  const uint32_t result = LockLevel(texture, face, tail_level > 0 ? uint32_t(tail_level) : 0, flags, address, pitch, slice, tail);
  StoreGuestU32(locked_tail, pitch);
  StoreGuestU32(locked_tail + 8, address - tail);
  StoreGuestU32(locked_tail + 4, slice);
  return result;
}

void D3DSurface_UnlockRect_hook(uint32_t surface) {
  const uint32_t texture = LoadGuestU32(surface + 0x18);
  UnlockResource(texture, LoadGuestU32(texture + 0x30) & ~0xFFFu);
}

uint32_t D3DDevice_CreateTexture_hook(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool, uint32_t type) {
  (void)pool;
  const uint32_t texture = cherry::core::CallGuest(GuestXMemAlloc, 0x34, kHeapAttributes);
  if (!texture) return 0;
  uint32_t base_size, mip_size;
  SetTextureHeader(type, width, height, depth, levels, usage, format, 2, 0, 0, 0, texture, base_size, mip_size);
  const uint32_t attributes = ((((~usage >> 2) & 1) | 2) << 28) | 0x8C800000;
  const uint32_t base_memory = cherry::core::CallGuest(GuestXMemAlloc, base_size, attributes);
  if (!base_memory) {
    cherry::core::CallGuest(GuestXMemFree, texture, 0x24800000);
    return 0;
  }
  uint32_t mip_memory = 0;
  if (mip_size) {
    mip_memory = cherry::core::CallGuest(GuestXMemAlloc, mip_size, attributes);
    if (!mip_memory) {
      cherry::core::CallGuest(GuestXMemFree, texture, 0x24800000);
      cherry::core::CallGuest(GuestXMemFree, base_memory, 0xB1800000);
      return 0;
    }
  }
  StoreGuestU32(texture + 0x20, (LoadGuestU32(texture + 0x20) & 0xFFF) | (base_memory & 0xFFFFF000));
  StoreGuestU32(texture + 0x30, (LoadGuestU32(texture + 0x30) & 0xFFF) | (mip_memory & 0xFFFFF000));
  return texture;
}

uint32_t D3DDevice_CreateSurface_hook(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, uint32_t params) {
  const uint32_t surface = cherry::core::CallGuest(GuestXMemAlloc, 0x30, kHeapAttributes);
  if (!surface) return 0;
  const uint32_t tiles = InitSurfaceHeader(width, height, format, multisample, params, surface);
  StoreGuestU32(surface, LoadGuestU32(surface) | 0x100000);
  if (params) return surface;
  const uint32_t fmt = LoadGuestU32(surface + 0x28) & 0x3F;
  const bool depth = fmt == 0x16 || fmt == 0x17;
  uint32_t tile_base;
  if (!EdramAlloc(tiles, tile_base)) {
    cherry::core::CallGuest(GuestXMemFree, surface, 0x24800000);
    return 0;
  }
  if (tile_base + tiles > 0x800) {
    const uint32_t allocator = LoadGuestU32(kEdramAllocator);
    cherry::core::CallGuest(GuestEnterCriticalSection, allocator);
    StoreGuestU32(allocator + 0x1C, LoadGuestU32(allocator + 0x1C) + tiles);
    EdramMark(allocator, tile_base, tile_base + tiles, false);
    cherry::core::CallGuest(GuestLeaveCriticalSection, allocator);
    cherry::core::CallGuest(GuestXMemFree, surface, 0x24800000);
    return 0;
  }
  StoreGuestU32(surface, LoadGuestU32(surface) | 0x80000000);
  StoreGuestU32(surface + 0x1C, (LoadGuestU32(surface + 0x1C) & ~0xFFFu) | (tile_base & 0xFFF));
  if (depth && !LoadGuestU32(kHiZClaimed)) {
    StoreGuestU32(surface + 0x20, LoadGuestU32(surface + 0x20) & 0x1FFFF);
    StoreGuestU32(kHiZClaimed, 1);
  }
  return surface;
}

void D3DTexture_GetLevelDesc_hook(uint32_t texture, uint32_t level, uint32_t desc) {
  TextureLevelDesc(texture, level, desc);
}

void D3DVolumeTexture_GetLevelDesc_hook(uint32_t texture, uint32_t level, uint32_t desc) {
  VolumeLevelDesc(texture, level, desc);
}

uint32_t D3DTexture_GetSurfaceLevel_hook(uint32_t texture, uint32_t level) {
  return NewChild(0x30, texture, 0x003FFFFF, level << 28);
}

uint32_t D3DCubeTexture_GetCubeMapSurface_hook(uint32_t texture, uint32_t face, uint32_t level) {
  return NewChild(0x30, texture, 0x003FFFFF, (level << 28) | ((face & 0x3F) << 22));
}

uint32_t D3DVolumeTexture_GetVolumeLevel_hook(uint32_t texture, uint32_t level) {
  return NewChild(0x20, texture, 0x0FFFFFFF, level << 28);
}

void D3DSurface_GetDesc_hook(uint32_t surface, uint32_t desc) {
  if (LoadGuestU32(surface) & 0x40000000) {
    TextureLevelDesc(LoadGuestU32(surface + 0x18), LoadGuestU32(surface + 0x1C) >> 28, desc);
  } else {
    StoreGuestU32(desc + 8, 0);
    StoreGuestU32(desc + 0xC, 0);
    StoreGuestU32(desc + 0x14, 0);
    StoreGuestU32(desc, LoadGuestU32(surface + 0x28));
    const uint32_t size = LoadGuestU32(surface + 0x24);
    StoreGuestU32(desc + 0x18, (size >> 18) + 1);
    StoreGuestU32(desc + 0x1C, ((size >> 3) & 0x7FFF) + 1);
    StoreGuestU32(desc + 0x10, (LoadGuestU32(surface + 0x18) >> 16) & 3);
  }
  StoreGuestU32(desc + 4, 4);
}

uint32_t D3DSurface_LockRect_hook(uint32_t surface, uint32_t locked_rect, uint32_t rect, uint32_t flags) {
  const uint32_t info = LoadGuestU32(surface + 0x1C);
  return LockRect(LoadGuestU32(surface + 0x18), (info >> 22) & 0x3F, info >> 28, locked_rect, rect, flags);
}

void D3DVolume_GetDesc_hook(uint32_t volume, uint32_t desc) {
  VolumeLevelDesc(LoadGuestU32(volume + 0x18), LoadGuestU32(volume + 0x1C) >> 28, desc);
  StoreGuestU32(desc + 4, 0x10);
}

uint32_t D3DVolume_LockBox_hook(uint32_t volume, uint32_t locked_box, uint32_t box, uint32_t flags) {
  return LockBox(LoadGuestU32(volume + 0x18), LoadGuestU32(volume + 0x1C) >> 28, locked_box, box, flags);
}

}  // namespace

// D3DDevice_SetBlendState(D3DDevice*, DWORD RenderTargetIndex, DWORD BlendState)
REX_HOOK(D3DDevice_SetBlendState, D3DDevice_SetBlendState_hook)
// XGGetBlockDimensions(DWORD Format, UINT* pWidth, UINT* pHeight) -> Format
REX_HOOK(XGGetBlockDimensions, XGGetBlockDimensions_hook)
// XGPadMipLevel(UINT* pWidth, UINT* pHeight, UINT* pDepth, DWORD BitsPerTexel, DWORD Format, DWORD Dimension, BOOL Tiled) -> level bytes
REX_HOOK(XGPadMipLevel, XGPadMipLevel_hook)
// XGCalcTextureLayout(Width, Height, Depth, Levels, Format, Dimension, Tiled, PackedMips, Border, ExpBias, Pitch, pAlignedWidth, pBaseSize, pMipSize) -> Levels
REX_HOOK(XGCalcTextureLayout, XGCalcTextureLayout_hook)
// D3DBaseTexture_GetDimensions(D3DBaseTexture*, UINT* pWidth, UINT* pHeight, UINT* pDepth) -> texture
REX_HOOK(D3DBaseTexture_GetDimensions, D3DBaseTexture_GetDimensions_hook)
// XGGetFetchConstantDimensions(const GPUTEXTURE_FETCH_CONSTANT*, UINT* pWidth, UINT* pHeight, UINT* pDepth) -> fetch
REX_HOOK(XGGetFetchConstantDimensions, XGGetFetchConstantDimensions_hook)
// XGGetMipTailLevelOffset(TailLevel, Width, Height, Depth, RowPitch, SlicePitch, Format, pX, pY, pZ) -> byte offset
REX_HOOK(XGGetMipTailLevelOffset, XGGetMipTailLevelOffset_hook)
// D3DBaseTexture_GetMipLayout(D3DBaseTexture*, Face, Level, pRowPitch, pSlicePitch, pLevelSize, pTailOffset, pWidth, pHeight, pDepth, pAddress)
REX_HOOK(D3DBaseTexture_GetMipLayout, D3DBaseTexture_GetMipLayout_hook)
// D3DBaseTexture_LockLevel(D3DBaseTexture*, Face, Level, Flags, pAddress, pRowPitch, pSlicePitch, pTailOffset) -> lock address
REX_HOOK(D3DBaseTexture_LockLevel, D3DBaseTexture_LockLevel_hook)
// D3DBaseTexture_LockRect(D3DBaseTexture*, Face, Level, D3DLOCKED_RECT*, const RECT*, Flags) -> lock address
REX_HOOK(D3DBaseTexture_LockRect, D3DBaseTexture_LockRect_hook)
// D3DVolumeTexture_LockBox(D3DVolumeTexture*, Level, D3DLOCKED_BOX*, const D3DBOX*, Flags) -> lock address
REX_HOOK(D3DVolumeTexture_LockBox, D3DVolumeTexture_LockBox_hook)
// XGSetTextureHeaderInternal(Type, Width, Height, Depth, Levels, Usage, Format, PackedMips, Border, ExpBias, Pitch, pTexture, pBaseSize, pMipSize)
REX_HOOK(XGSetTextureHeaderInternal, XGSetTextureHeaderInternal_hook)
// D3DBaseTexture_GetLevelCount(D3DBaseTexture*) -> levels
REX_HOOK(D3DBaseTexture_GetLevelCount, D3DBaseTexture_GetLevelCount_hook)
// D3DBaseTexture_LockTail(D3DBaseTexture*, ArrayIndex, D3DLOCKED_TAIL*, Flags) -> lock address
REX_HOOK(D3DBaseTexture_LockTail, D3DBaseTexture_LockTail_hook)
// D3DSurface_UnlockRect(D3DSurface*)
REX_HOOK(D3DSurface_UnlockRect, D3DSurface_UnlockRect_hook)
// D3DDevice_CreateTexture(Width, Height, Depth, Levels, Usage, Format, Pool, Type) -> D3DBaseTexture*
REX_HOOK(D3DDevice_CreateTexture, D3DDevice_CreateTexture_hook)
// D3DDevice_CreateSurface(Width, Height, Format, MultiSample, const D3DSURFACE_PARAMETERS*) -> D3DSurface*
REX_HOOK(D3DDevice_CreateSurface, D3DDevice_CreateSurface_hook)
// D3DTexture_GetLevelDesc(D3DTexture*, Level, D3DSURFACE_DESC*)
REX_HOOK(D3DTexture_GetLevelDesc, D3DTexture_GetLevelDesc_hook)
// D3DVolumeTexture_GetLevelDesc(D3DVolumeTexture*, Level, D3DVOLUME_DESC*)
REX_HOOK(D3DVolumeTexture_GetLevelDesc, D3DVolumeTexture_GetLevelDesc_hook)
// D3DTexture_GetSurfaceLevel(D3DTexture*, Level) -> D3DSurface*
REX_HOOK(D3DTexture_GetSurfaceLevel, D3DTexture_GetSurfaceLevel_hook)
// D3DCubeTexture_GetCubeMapSurface(D3DCubeTexture*, FaceType, Level) -> D3DSurface*
REX_HOOK(D3DCubeTexture_GetCubeMapSurface, D3DCubeTexture_GetCubeMapSurface_hook)
// D3DVolumeTexture_GetVolumeLevel(D3DVolumeTexture*, Level) -> D3DVolume*
REX_HOOK(D3DVolumeTexture_GetVolumeLevel, D3DVolumeTexture_GetVolumeLevel_hook)
// D3DSurface_GetDesc(D3DSurface*, D3DSURFACE_DESC*)
REX_HOOK(D3DSurface_GetDesc, D3DSurface_GetDesc_hook)
// D3DSurface_LockRect(D3DSurface*, D3DLOCKED_RECT*, const RECT*, Flags) -> lock address
REX_HOOK(D3DSurface_LockRect, D3DSurface_LockRect_hook)
// D3DVolume_GetDesc(D3DVolume*, D3DVOLUME_DESC*)
REX_HOOK(D3DVolume_GetDesc, D3DVolume_GetDesc_hook)
// D3DVolume_LockBox(D3DVolume*, D3DLOCKED_BOX*, const D3DBOX*, Flags) -> lock address
REX_HOOK(D3DVolume_LockBox, D3DVolume_LockBox_hook)
