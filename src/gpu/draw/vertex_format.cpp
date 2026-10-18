#include "gpu/draw/vertex_format.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace cherry::gpu {

namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// The dword at `p` after the fetch constant's swap (0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32); fields are read from bit 0 up, which Bind's selector XOR undoes for 16-bit fields.
uint32_t GpuDword(const uint8_t* p, uint32_t endian) {
  switch (endian & 3) {
    case 0: return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    case 1: return uint32_t(p[1]) | (uint32_t(p[0]) << 8) | (uint32_t(p[3]) << 16) | (uint32_t(p[2]) << 24);
    case 3: return uint32_t(p[2]) | (uint32_t(p[3]) << 8) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 24);
    default: return Be32(p);
  }
}

uint16_t GpuHalf(const uint8_t* src, uint32_t component, uint32_t endian) {
  return uint16_t(GpuDword(src + 4 * (component / 2), endian) >> (16 * (component & 1)));
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while (!(mantissa & 0x400)) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FF;
      bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

// One field of `bits` width read as the element's number format.
float Field(uint32_t value, uint32_t bits, uint32_t num_format) {
  const uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
  value &= mask;
  const bool is_signed = num_format == kVertexSNorm || num_format == kVertexSInt;
  int64_t v = value;
  if (is_signed && bits < 32 && (value & (1u << (bits - 1)))) v -= int64_t(1) << bits;
  if (is_signed && bits == 32) v = int32_t(value);
  switch (num_format) {
    case kVertexUNorm: return float(double(v) / double(mask));
    case kVertexSNorm: return std::max(-1.0f, float(double(v) / double(mask >> 1)));
    default: return float(v);
  }
}

}  // namespace

void DecodeVertexElement(const GuestVertexType& type, const uint8_t* src, uint32_t endian, float result[4]) {
  float out[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const uint32_t nf = type.num_format;
  switch (type.data_format) {
    case 6: {
      const uint32_t v = GpuDword(src, endian);
      for (int i = 0; i < 4; ++i) out[i] = Field(v >> (8 * i), 8, nf);
      break;
    }
    case 7: {
      const uint32_t v = GpuDword(src, endian);
      out[0] = Field(v, 10, nf);
      out[1] = Field(v >> 10, 10, nf);
      out[2] = Field(v >> 20, 10, nf);
      out[3] = Field(v >> 30, 2, nf);
      break;
    }
    case 16: {
      const uint32_t v = GpuDword(src, endian);
      out[0] = Field(v, 11, nf);
      out[1] = Field(v >> 11, 11, nf);
      out[2] = Field(v >> 22, 10, nf);
      break;
    }
    case 17: {
      const uint32_t v = GpuDword(src, endian);
      out[0] = Field(v, 10, nf);
      out[1] = Field(v >> 10, 11, nf);
      out[2] = Field(v >> 21, 11, nf);
      break;
    }
    case 25:
    case 26:
      for (uint32_t i = 0; i < type.components; ++i) out[i] = Field(GpuHalf(src, i, endian), 16, nf);
      break;
    case 31:
    case 32:
      for (uint32_t i = 0; i < type.components; ++i) out[i] = HalfToFloat(GpuHalf(src, i, endian));
      break;
    case 36:
    case 37:
    case 57:
    case 38:
      for (uint32_t i = 0; i < type.components; ++i) out[i] = std::bit_cast<float>(GpuDword(src + 4 * i, endian));
      break;
    case 33:
    case 34:
    case 35:
      for (uint32_t i = 0; i < type.components; ++i) out[i] = Field(GpuDword(src + 4 * i, endian), 32, nf);
      break;
    default:
      break;
  }
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t selector = (type.swizzle >> (3 * c)) & 7;
    result[c] = selector < 4 ? out[type.halves_swapped ? selector ^ 1 : selector] : selector == 4 ? 0.0f : 1.0f;
  }
}

}  // namespace cherry::gpu
