#include "ui/movie_smoothing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>

namespace cherry::ui {

namespace {

constexpr uint16_t kEnd = 0, kShowFrame = 1, kDoAction = 12, kPlaceObject2 = 26, kRemoveObject2 = 28, kRemoveObject = 5, kDefineSprite = 39, kDoInitAction = 59, kPlaceObject3 = 70;
constexpr uint16_t kSoundStreamHead = 18, kSoundStreamHead2 = 45, kSoundStreamBlock = 19;

struct Tag {
  uint16_t code;
  size_t body;    // offset of the body in the movie
  uint32_t length;
  size_t start;   // offset of the header
};

bool ReadTag(const std::vector<uint8_t>& d, size_t pos, size_t end, Tag& tag) {
  if (pos + 2 > end) return false;
  const uint16_t word = uint16_t(d[pos] | (d[pos + 1] << 8));
  tag.code = word >> 6;
  tag.length = word & 0x3F;
  tag.start = pos;
  tag.body = pos + 2;
  if (tag.length == 0x3F) {
    if (pos + 6 > end) return false;
    tag.length = uint32_t(d[pos + 2]) | (uint32_t(d[pos + 3]) << 8) | (uint32_t(d[pos + 4]) << 16) | (uint32_t(d[pos + 5]) << 24);
    tag.body = pos + 6;
  }
  return tag.body + tag.length <= end;
}

void AppendTag(std::vector<uint8_t>& out, uint16_t code, const uint8_t* body, size_t length) {
  if (length < 0x3F) {
    const uint16_t word = uint16_t((code << 6) | length);
    out.push_back(uint8_t(word));
    out.push_back(uint8_t(word >> 8));
  } else {
    const uint16_t word = uint16_t((code << 6) | 0x3F);
    out.push_back(uint8_t(word));
    out.push_back(uint8_t(word >> 8));
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(uint32_t(length) >> (8 * i)));
  }
  out.insert(out.end(), body, body + length);
}

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  uint32_t Bits(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t byte = bit_ >> 3;
      const uint32_t b = byte < size_ ? (data_[byte] >> (7 - (bit_ & 7))) & 1 : 0;
      v = (v << 1) | b;
      ++bit_;
    }
    return v;
  }
  int32_t Signed(uint32_t n) {
    if (!n) return 0;
    const uint32_t v = Bits(n);
    return (v & (1u << (n - 1))) ? int32_t(v | ~((1u << n) - 1)) : int32_t(v);
  }
  size_t Bytes() const { return (bit_ + 7) >> 3; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t bit_ = 0;
};

class BitWriter {
 public:
  void Bits(uint32_t v, uint32_t n) {
    for (int32_t i = int32_t(n) - 1; i >= 0; --i) {
      if ((bit_ & 7) == 0) out.push_back(0);
      if ((v >> i) & 1) out.back() |= uint8_t(0x80 >> (bit_ & 7));
      ++bit_;
    }
  }
  std::vector<uint8_t> out;

 private:
  size_t bit_ = 0;
};

uint32_t SignedBits(std::initializer_list<int32_t> values) {
  uint32_t bits = 1;
  for (int32_t v : values) {
    uint32_t n = 1;
    while (n < 32 && (v < -(int32_t(1) << (n - 1)) || v > (int32_t(1) << (n - 1)) - 1)) ++n;
    bits = std::max(bits, n);
  }
  return bits;
}

struct Matrix {
  int32_t sx = 65536, sy = 65536, r0 = 0, r1 = 0, tx = 0, ty = 0;
  bool operator==(const Matrix& o) const { return sx == o.sx && sy == o.sy && r0 == o.r0 && r1 == o.r1 && tx == o.tx && ty == o.ty; }
};

Matrix ReadMatrix(BitReader& r) {
  Matrix m;
  if (r.Bits(1)) {
    const uint32_t n = r.Bits(5);
    m.sx = r.Signed(n);
    m.sy = r.Signed(n);
  }
  if (r.Bits(1)) {
    const uint32_t n = r.Bits(5);
    m.r0 = r.Signed(n);
    m.r1 = r.Signed(n);
  }
  const uint32_t n = r.Bits(5);
  m.tx = r.Signed(n);
  m.ty = r.Signed(n);
  return m;
}

void WriteMatrix(BitWriter& w, const Matrix& m) {
  const bool scale = m.sx != 65536 || m.sy != 65536;
  w.Bits(scale, 1);
  if (scale) {
    const uint32_t n = SignedBits({m.sx, m.sy});
    w.Bits(n, 5);
    w.Bits(uint32_t(m.sx), n);
    w.Bits(uint32_t(m.sy), n);
  }
  const bool rotate = m.r0 != 0 || m.r1 != 0;
  w.Bits(rotate, 1);
  if (rotate) {
    const uint32_t n = SignedBits({m.r0, m.r1});
    w.Bits(n, 5);
    w.Bits(uint32_t(m.r0), n);
    w.Bits(uint32_t(m.r1), n);
  }
  const uint32_t n = (m.tx == 0 && m.ty == 0) ? 0 : SignedBits({m.tx, m.ty});
  w.Bits(n, 5);
  w.Bits(uint32_t(m.tx), n);
  w.Bits(uint32_t(m.ty), n);
}

// CXFORMWITHALPHA: multipliers in 8.8 (256 is 1), additions in -255..255.
struct ColorTransform {
  int32_t mul[4] = {256, 256, 256, 256};
  int32_t add[4] = {0, 0, 0, 0};
  bool operator==(const ColorTransform& o) const { return std::equal(mul, mul + 4, o.mul) && std::equal(add, add + 4, o.add); }
};

ColorTransform ReadColorTransform(BitReader& r) {
  ColorTransform c;
  const bool has_add = r.Bits(1);
  const bool has_mul = r.Bits(1);
  const uint32_t n = r.Bits(4);
  if (has_mul) for (int i = 0; i < 4; ++i) c.mul[i] = r.Signed(n);
  if (has_add) for (int i = 0; i < 4; ++i) c.add[i] = r.Signed(n);
  return c;
}

void WriteColorTransform(BitWriter& w, const ColorTransform& c) {
  const bool has_mul = !std::all_of(c.mul, c.mul + 4, [](int32_t v) { return v == 256; });
  const bool has_add = !std::all_of(c.add, c.add + 4, [](int32_t v) { return v == 0; });
  uint32_t n = 1;
  if (has_mul) n = std::max(n, SignedBits({c.mul[0], c.mul[1], c.mul[2], c.mul[3]}));
  if (has_add) n = std::max(n, SignedBits({c.add[0], c.add[1], c.add[2], c.add[3]}));
  n = std::min(n, 15u);
  w.Bits(has_add, 1);
  w.Bits(has_mul, 1);
  w.Bits(n, 4);
  if (has_mul) for (int i = 0; i < 4; ++i) w.Bits(uint32_t(c.mul[i]), n);
  if (has_add) for (int i = 0; i < 4; ++i) w.Bits(uint32_t(c.add[i]), n);
}

struct Placed {
  uint16_t character = 0;
  Matrix matrix;
  ColorTransform color;
};

using DisplayList = std::map<uint16_t, Placed>;

// Applies a PlaceObject2/3 to the display list; returns false for a tag it cannot follow.
bool ApplyPlace(const std::vector<uint8_t>& d, const Tag& tag, DisplayList& list) {
  size_t p = tag.body;
  const size_t end = tag.body + tag.length;
  const uint8_t flags = d[p++];
  uint8_t flags2 = 0;
  if (tag.code == kPlaceObject3) flags2 = d[p++];
  if (p + 2 > end) return false;
  const uint16_t depth = uint16_t(d[p] | (d[p + 1] << 8));
  p += 2;
  // PlaceObject3's class name comes before the character id.
  if (tag.code == kPlaceObject3 && ((flags2 & 0x08) || ((flags2 & 0x10) && (flags & 0x02)))) {
    while (p < end && d[p]) ++p;
    ++p;
  }
  const bool move = flags & 0x01;
  Placed placed = move && list.count(depth) ? list[depth] : Placed{};
  if (flags & 0x02) {
    if (p + 2 > end) return false;
    placed.character = uint16_t(d[p] | (d[p + 1] << 8));
    p += 2;
    if (move) {
      placed.matrix = Matrix{};
      placed.color = ColorTransform{};
    }
  }
  BitReader r(d.data() + p, end - p);
  if (flags & 0x04) placed.matrix = ReadMatrix(r);
  if (flags & 0x08) {
    BitReader rc(d.data() + p + r.Bytes(), end - p - r.Bytes());
    placed.color = ReadColorTransform(rc);
  }
  list[depth] = placed;
  return true;
}

uint16_t RemovedDepth(const std::vector<uint8_t>& d, const Tag& tag) {
  const size_t p = tag.code == kRemoveObject ? tag.body + 2 : tag.body;
  return uint16_t(d[p] | (d[p + 1] << 8));
}

int32_t Lerp(int32_t a, int32_t b) {
  return int32_t(std::lround((double(a) + double(b)) * 0.5));
}

// A PlaceObject2 that moves the object at `depth` halfway from `a` to `b`.
std::vector<uint8_t> HalfwayPlace(uint16_t depth, const Placed& a, const Placed& b) {
  Matrix m;
  m.sx = Lerp(a.matrix.sx, b.matrix.sx);
  m.sy = Lerp(a.matrix.sy, b.matrix.sy);
  m.r0 = Lerp(a.matrix.r0, b.matrix.r0);
  m.r1 = Lerp(a.matrix.r1, b.matrix.r1);
  m.tx = Lerp(a.matrix.tx, b.matrix.tx);
  m.ty = Lerp(a.matrix.ty, b.matrix.ty);
  const bool color = !(a.color == b.color);
  std::vector<uint8_t> body = {uint8_t(0x01 | 0x04 | (color ? 0x08 : 0)), uint8_t(depth), uint8_t(depth >> 8)};
  BitWriter wm;
  WriteMatrix(wm, m);
  body.insert(body.end(), wm.out.begin(), wm.out.end());
  if (color) {
    ColorTransform c;
    for (int i = 0; i < 4; ++i) {
      c.mul[i] = Lerp(a.color.mul[i], b.color.mul[i]);
      c.add[i] = Lerp(a.color.add[i], b.color.add[i]);
    }
    BitWriter wc;
    WriteColorTransform(wc, c);
    body.insert(body.end(), wc.out.begin(), wc.out.end());
  }
  return body;
}

// Whether a PlaceObject2's clip events include enterFrame, which runs once per frame; the construct and load events CLIK components use to pass parameters are harmless.
bool ClipEventsEnterFrame(const std::vector<uint8_t>& d, const Tag& tag) {
  size_t p = tag.body;
  const size_t end = tag.body + tag.length;
  const uint8_t flags = d[p++];
  if (!(flags & 0x80)) return false;
  p += 2;  // depth
  if (flags & 0x02) p += 2;
  if (flags & 0x04) {
    BitReader r(d.data() + p, end - p);
    ReadMatrix(r);
    p += r.Bytes();
  }
  if (flags & 0x08) {
    BitReader r(d.data() + p, end - p);
    ReadColorTransform(r);
    p += r.Bytes();
  }
  if (flags & 0x10) p += 2;
  if (flags & 0x20) {
    while (p < end && d[p]) ++p;
    ++p;
  }
  if (flags & 0x40) p += 2;
  // CLIPACTIONS: reserved u16, then AllEventFlags, whose first byte has EnterFrame at bit 1.
  p += 2;
  return p >= end || (d[p] & 0x02) != 0;
}

// Whether action bytecode could depend on the frame count: frame-number jumps (GotoFrame, GotoFrame2).
bool ActionsCountFrames(const uint8_t* p, size_t size) {
  size_t i = 0;
  while (i < size) {
    const uint8_t op = p[i++];
    if (op == 0) break;
    if (op == 0x81 || op == 0x9F) return true;
    if (op >= 0x80) {
      if (i + 2 > size) return true;
      i += 2 + size_t(p[i] | (p[i + 1] << 8));
    }
  }
  return false;
}

struct Unsafe {};

// Rewrites one timeline (the root's tags or a sprite's), doubling its frames; `frames` is updated to the new count.
std::vector<uint8_t> SmoothTimeline(const std::vector<uint8_t>& d, size_t pos, size_t end, uint16_t& frames) {
  std::vector<std::vector<Tag>> timeline(1);
  Tag tag;
  while (ReadTag(d, pos, end, tag)) {
    pos = tag.body + tag.length;
    if (tag.code == kEnd) break;
    if (tag.code == kSoundStreamHead || tag.code == kSoundStreamHead2 || tag.code == kSoundStreamBlock) throw Unsafe{};
    if ((tag.code == kDoAction || tag.code == kDoInitAction) && ActionsCountFrames(d.data() + tag.body + (tag.code == kDoInitAction ? 2 : 0), tag.length)) throw Unsafe{};
    if (tag.code == kPlaceObject3 && (d[tag.body] & 0x80)) throw Unsafe{};
    if (tag.code == kPlaceObject2 && ClipEventsEnterFrame(d, tag)) throw Unsafe{};
    timeline.back().push_back(tag);
    if (tag.code == kShowFrame) timeline.emplace_back();
  }
  if (!timeline.back().empty()) timeline.back().push_back(Tag{kShowFrame, 0, 0, 0});
  else timeline.pop_back();

  // The display list after each frame.
  std::vector<DisplayList> after(timeline.size());
  DisplayList list;
  for (size_t f = 0; f < timeline.size(); ++f) {
    for (const Tag& t : timeline[f]) {
      if (t.code == kPlaceObject2 || t.code == kPlaceObject3) {
        if (!ApplyPlace(d, t, list)) throw Unsafe{};
      } else if (t.code == kRemoveObject || t.code == kRemoveObject2) {
        list.erase(RemovedDepth(d, t));
      }
    }
    after[f] = list;
  }

  std::vector<uint8_t> out;
  for (size_t f = 0; f < timeline.size(); ++f) {
    for (const Tag& t : timeline[f]) {
      if (t.code == kShowFrame) break;
      if (t.code == kDefineSprite) {
        // A sprite's own timeline is smoothed where it is defined.
        uint16_t sprite_frames = uint16_t(d[t.body + 2] | (d[t.body + 3] << 8));
        std::vector<uint8_t> body(d.begin() + t.body, d.begin() + t.body + 4);
        const std::vector<uint8_t> inner = SmoothTimeline(d, t.body + 4, t.body + t.length, sprite_frames);
        body[2] = uint8_t(sprite_frames);
        body[3] = uint8_t(sprite_frames >> 8);
        body.insert(body.end(), inner.begin(), inner.end());
        AppendTag(out, kDefineSprite, body.data(), body.size());
        continue;
      }
      AppendTag(out, t.code, d.data() + t.body, t.length);
    }
    AppendTag(out, kShowFrame, nullptr, 0);
    // The in-between frame: objects that stay on screen into the next frame as the same character move halfway there.
    if (f + 1 < timeline.size()) {
      for (const auto& [depth, now] : after[f]) {
        auto next = after[f + 1].find(depth);
        if (next == after[f + 1].end() || next->second.character != now.character) continue;
        if (now.matrix == next->second.matrix && now.color == next->second.color) continue;
        const std::vector<uint8_t> body = HalfwayPlace(depth, now, next->second);
        AppendTag(out, kPlaceObject2, body.data(), body.size());
      }
    }
    AppendTag(out, kShowFrame, nullptr, 0);
  }
  AppendTag(out, kEnd, nullptr, 0);
  frames = uint16_t(std::min<size_t>(timeline.size() * 2, 0xFFFF));
  return out;
}

bool Contains(const std::vector<uint8_t>& d, std::string_view text) {
  return std::search(d.begin(), d.end(), text.begin(), text.end()) != d.end();
}

}  // namespace

std::vector<uint8_t> SmoothMovieTimeline(const std::vector<uint8_t>& movie) {
  if (movie.size() < 16 || std::memcmp(movie.data(), "GFX", 3) != 0) return {};
  if (Contains(movie, "onEnterFrame") || Contains(movie, "_currentframe")) return {};
  const uint32_t rect_bits = 5 + 4 * (movie[8] >> 3);
  const size_t header = 8 + (rect_bits + 7) / 8;
  if (header + 4 > movie.size()) return {};
  try {
    uint16_t frames = uint16_t(movie[header + 2] | (movie[header + 3] << 8));
    const std::vector<uint8_t> tags = SmoothTimeline(movie, header + 4, movie.size(), frames);
    std::vector<uint8_t> out(movie.begin(), movie.begin() + header + 4);
    // FRAMERATE is 8.8 fixed point, little-endian.
    const uint16_t rate = uint16_t(out[header] | (out[header + 1] << 8));
    const uint16_t doubled = uint16_t(std::min<uint32_t>(uint32_t(rate) * 2, 0xFFFF));
    out[header] = uint8_t(doubled);
    out[header + 1] = uint8_t(doubled >> 8);
    out[header + 2] = uint8_t(frames);
    out[header + 3] = uint8_t(frames >> 8);
    out.insert(out.end(), tags.begin(), tags.end());
    const uint32_t size = uint32_t(out.size());
    for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t(size >> (8 * i));
    return out;
  } catch (const Unsafe&) {
    return {};
  }
}

}  // namespace cherry::ui
