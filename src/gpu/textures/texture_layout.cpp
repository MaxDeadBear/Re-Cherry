#include "gpu/textures/texture_layout.h"

#include <bit>

namespace cherry::gpu {

namespace {

// Per format {flags, bits per texel}, copied from 0x821A54F0.
constexpr uint8_t kFormatTable[128] = {
    0x00, 0x01, 0x00, 0x01, 0x20, 0x08, 0x20, 0x10, 0x20, 0x10, 0x20, 0x10, 0x30, 0x20, 0x32, 0x20,
    0x20, 0x08, 0x20, 0x08, 0x20, 0x10, 0x00, 0x10, 0x00, 0x10, 0x14, 0x20, 0x20, 0x20, 0x20, 0x10,
    0x20, 0x20, 0x20, 0x20, 0x00, 0x04, 0x00, 0x08, 0x00, 0x08, 0x15, 0x40, 0x10, 0x20, 0x11, 0x20,
    0x20, 0x10, 0x20, 0x20, 0x20, 0x40, 0x00, 0x10, 0x00, 0x20, 0x00, 0x40, 0x20, 0x10, 0x36, 0x20,
    0x37, 0x40, 0x00, 0x20, 0x00, 0x40, 0x00, 0x80, 0x3E, 0x20, 0x3F, 0x40, 0x20, 0x80, 0x00, 0x08,
    0x00, 0x10, 0x00, 0x10, 0x00, 0x20, 0x00, 0x08, 0x00, 0x08, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10,
    0x00, 0x20, 0x00, 0x08, 0x00, 0x20, 0x00, 0x04, 0x00, 0x08, 0x00, 0x08, 0x12, 0x20, 0x00, 0x20,
    0x00, 0x20, 0x00, 0x60, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x11, 0x20, 0x13, 0x20};

uint32_t Cntlzw(uint32_t v) { return uint32_t(std::countl_zero(v)); }
uint32_t Slw(uint32_t v, uint32_t n) { return (n & 0x20) ? 0 : v << (n & 0x1F); }
uint32_t Srw(uint32_t v, uint32_t n) { return (n & 0x20) ? 0 : v >> (n & 0x1F); }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
uint32_t Page(uint32_t v) { return (v + 0xFFF) & ~0xFFFu; }
uint32_t AtLeastOne(uint32_t v) { return v > 1 ? v : 1; }

}  // namespace

uint32_t FormatBitsPerTexel(uint32_t format) {
  return kFormatTable[(format & 0x3F) * 2 + 1];
}

uint32_t FormatFlags(uint32_t format) {
  return kFormatTable[(format & 0x3F) * 2] & 0xF;
}

void FormatBlockSize(uint32_t format, uint32_t& block_width, uint32_t& block_height) {
  if (format == 0x2C || format == 0x27) {
    block_width = 4;
    block_height = 1;
    return;
  }
  if (format == 0x0B || format == 0x0C || format == 0x28) {
    block_width = 2;
    block_height = 1;
    return;
  }
  const bool block = (format >= 0x12 && format <= 0x14) || format == 0x31 || (format >= 0x33 && format <= 0x35) || (format >= 0x3A && format <= 0x3D);
  block_width = block_height = block ? 4 : 1;
}

uint32_t PadMipLevel(uint32_t& width, uint32_t& height, uint32_t& depth, uint32_t bpp, uint32_t format, uint32_t dimension, uint32_t tiled) {
  const uint32_t align_h = dimension == 0 ? 1 : 32;
  const uint32_t align_d = dimension == 2 ? 4 : 1;
  uint32_t align_w = 32, bw, bh;
  FormatBlockSize(format, bw, bh);
  if (!tiled) {
    const uint32_t block_bytes = (bw * bh * bpp) >> 3;
    if (block_bytes && 0x100 / block_bytes >= 32) align_w = 0x100 / block_bytes;
  }
  width = AlignUp(width, bw * align_w);
  height = AlignUp(height, bh * align_h);
  depth = AlignUp(depth, align_d);
  const uint32_t slice = ((width * bpp) >> 3) * height;
  return dimension == 2 ? Page(depth * slice) : Page(slice) * depth;
}

uint32_t MipTailOffset(uint32_t level, uint32_t width, uint32_t height, uint32_t depth, uint32_t row_pitch, uint32_t slice_pitch, uint32_t format, uint32_t& x, uint32_t& y, uint32_t& z) {
  uint32_t bw, bh;
  FormatBlockSize(format, bw, bh);
  const uint32_t log_w = 32 - Cntlzw(width - 1), log_h = 32 - Cntlzw(height - 1), log_d = 32 - Cntlzw(depth - 1);
  const uint32_t dims[2] = {Slw(1, log_w), Slw(1, log_h)};
  uint32_t xy[2] = {0, 0}, offset_z = 0, axis = log_h < log_w ? 1 : 0;
  if (level < 3) {
    xy[axis] = 16u >> level;
  } else {
    axis ^= 1;
    xy[axis] = Srw(dims[axis], level - 2);
    if (xy[axis] < 4) offset_z = 4 * AtLeastOne(log_d - level);
  }
  x = xy[0] / bw;
  y = xy[1] / bh;
  z = offset_z;
  return ((bw * FormatBitsPerTexel(format) * xy[0]) >> 3) + xy[1] * row_pitch + offset_z * slice_pitch;
}

MipLocation LocateMip(const uint32_t fetch[6], uint32_t level, uint32_t layer) {
  const uint32_t d0 = fetch[0], d1 = fetch[1], d2 = fetch[2], d3 = fetch[3], d4 = fetch[4], d5 = fetch[5];
  const uint32_t border = d3 >> 31, b2 = border * 2, bpp = FormatBitsPerTexel(d1), tiled = d0 >> 31, dimension = (d5 >> 9) & 3, format = d1 & 0x3F;
  const uint32_t add = ((d3 >> 30) & 2) + 1;
  uint32_t width, height, depth;
  if (dimension == 0) {
    width = (d2 & 0xFFFFFF) + add;
    height = depth = 1;
  } else if (dimension == 2) {
    width = (d2 & 0x7FF) + add;
    height = ((d2 >> 11) & 0x7FF) + add;
    depth = (d2 >> 22) + add;
  } else {
    width = (d2 & 0x1FFF) + add;
    height = ((d2 >> 13) & 0x1FFF) + add;
    depth = 1;
  }
  MipLocation m;
  const uint32_t w_in = width - b2, h_in = height - b2;
  const uint32_t pw = Slw(1, border - Cntlzw(w_in - 1) + 32);
  uint32_t rows = Slw(1, border - Cntlzw(h_in - 1) + 32);
  uint32_t slices = dimension == 2 ? Slw(1, border - Cntlzw(depth - b2 - 1) + 32) : 1;
  const bool packed = !(pw > 16 && rows > 16) && (d5 & 0x800);
  uint32_t address = (level == 0 || packed) ? (d1 & ~0xFFFu) : (d5 & ~0xFFFu);
  m.width = AtLeastOne(Srw(w_in, level)) + b2;
  m.height = AtLeastOne(Srw(h_in, level)) + b2;
  if (level == 0 && !packed) {
    uint32_t w = width, h = height, d = depth;
    PadMipLevel(w, h, d, bpp, format, dimension, tiled);
    m.rows = h;
    if (!tiled && !(d1 & 0x400) && !(d5 & 0x800) && dimension == 1 && !(d4 & 0x3C0) && !border) {
      uint32_t bw, bh;
      FormatBlockSize(format, bw, bh);
      m.rows = AlignUp(height, bh);
    }
    m.row_pitch = dimension == 0 ? (w * bpp) >> 3 : ((((d0 >> 22) & 0x1FF) * bpp) << 2) & 0x1FFFFFFC;
    m.address = address + (dimension == 2 ? 0 : Page(m.row_pitch * m.rows) * layer);
    return m;
  }
  const uint32_t type_layers = dimension == 3 ? 0x12 : (dimension == 1 && (d1 & 0x400)) ? 0x13 : 0;
  const uint32_t align = Slw(1, (d1 >> 9) & 2);
  const uint32_t layers = type_layers ? ((d2 >> 26) + align) & ~(align - 1) : 1;
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
    PadMipLevel(w, h, d, bpp, format, dimension, tiled);
    m.row_pitch = (w * bpp) >> 3;
    m.rows = h;
    slices = d;
    if (!(lw > 16 && lh > 16) && (d5 & 0x800)) {
      uint32_t z;
      MipTailOffset(uint32_t(i), lw, lh, ld, m.row_pitch, m.row_pitch * h, format, m.tail_x, m.tail_y, z);
      m.in_tail = true;
      break;
    }
    if (i > 0) address += (dimension == 2 ? Page(m.row_pitch * slices * h) : Page(m.row_pitch * h) * slices) * layers;
  }
  m.address = address + (dimension == 2 ? 0 : Page(m.row_pitch * m.rows) * layer);
  return m;
}

}  // namespace cherry::gpu
