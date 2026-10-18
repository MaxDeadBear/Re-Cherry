// Guest vertex declarations: the Xbox D3DVERTEXELEMENT9 encoding, and how each element reaches the translated shader's inputs.
#pragma once

#include <cstdint>

namespace cherry::gpu {

// D3DDECLUSAGE, in the order XenosRecomp's USAGE_SEMANTICS uses.
enum GuestDeclUsage : uint32_t {
  kUsagePosition = 0,
  kUsageBlendWeight = 1,
  kUsageBlendIndices = 2,
  kUsageNormal = 3,
  kUsagePointSize = 4,
  kUsageTexCoord = 5,
  kUsageTangent = 6,
  kUsageBinormal = 7,
  kUsageTessFactor = 8,
  kUsagePositionT = 9,
  kUsageColor = 10,
  kUsageFog = 11,
  kUsageDepth = 12,
  kUsageSample = 13,
  kUsageCount = 14,
};

// D3DDECLTYPE as D3DVertexShader_Bind (sub_829EC9C0) reads it: bits 0..5 the vertex data format, bit 8 signed, bit 9 integer, bits 10..21 four 3-bit component selectors (0..3 a component, 4 zero, 5 one), and bits 6..7 = 01 for formats of 16-bit fields, whose selectors Bind XORs with 1.
enum GuestVertexNumFormat : uint32_t {
  kVertexUNorm = 0,
  kVertexSNorm = 1,
  kVertexUInt = 2,
  kVertexSInt = 3,
};

struct GuestVertexType {
  uint32_t data_format = 0;
  uint32_t num_format = 0;
  uint32_t swizzle = 0;
  bool halves_swapped = false;
  uint32_t components = 0;
  uint32_t size = 0;  // bytes in guest memory, 0 for an unknown format
};

inline GuestVertexType DecodeVertexType(uint32_t decl_type) {
  GuestVertexType t;
  t.data_format = decl_type & 0x3F;
  t.num_format = (decl_type >> 8) & 3;
  t.swizzle = (decl_type >> 10) & 0xFFF;
  t.halves_swapped = (decl_type & 0xC0) == 0x40;
  switch (t.data_format) {
    case 6: t.components = 4; t.size = 4; break;    // 8_8_8_8: UBYTE4, UBYTE4N, D3DCOLOR
    case 7: t.components = 4; t.size = 4; break;    // 2_10_10_10: DEC3N, UDEC3
    case 16: t.components = 3; t.size = 4; break;   // 10_11_11: HEND3, X and Y 11 bits from bit 0, Z 10
    case 17: t.components = 3; t.size = 4; break;   // 11_11_10: DHEN3, X 10 bits from bit 0, Y and Z 11
    case 25: t.components = 2; t.size = 4; break;   // 16_16
    case 26: t.components = 4; t.size = 8; break;   // 16_16_16_16
    case 31: t.components = 2; t.size = 4; break;   // 16_16_FLOAT
    case 32: t.components = 4; t.size = 8; break;   // 16_16_16_16_FLOAT
    case 33: case 36: t.components = 1; t.size = 4; break;
    case 34: case 37: t.components = 2; t.size = 8; break;
    case 57: t.components = 3; t.size = 12; break;  // 32_32_32_FLOAT
    case 35: case 38: t.components = 4; t.size = 16; break;
    default: break;
  }
  return t;
}

// Must match XenosRecomp's USAGE_LOCATIONS (cherry.patch), because plume's Vulkan backend uses the location verbatim; unlisted elements take 23 onward in order.
inline bool DeclUsageLocation(uint32_t usage, uint32_t index, uint32_t* location) {
  switch (usage) {
    case kUsagePosition:
      if (index == 0) *location = 0;
      else if (index == 1) *location = 15;
      else if (index <= 7) *location = 15 + index;
      else return false;
      return true;
    case kUsageNormal: if (index) return false; *location = 1; return true;
    case kUsageTangent: if (index) return false; *location = 2; return true;
    case kUsageBinormal: if (index) return false; *location = 3; return true;
    case kUsageColor: if (index > 1) return false; *location = index == 0 ? 8 : 11; return true;
    case kUsageBlendIndices: if (index) return false; *location = 9; return true;
    case kUsageBlendWeight: if (index) return false; *location = 10; return true;
    case kUsageTexCoord:
      if (index <= 3) *location = 4 + index;
      else if (index <= 7) *location = 8 + index;
      else if (index == 8) *location = 16;
      else return false;
      return true;
    default: return false;
  }
}

inline const char* DeclUsageSemantic(uint32_t usage) {
  static const char* kSemantics[kUsageCount] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
  return usage < kUsageCount ? kSemantics[usage] : "TEXCOORD";
}

// XenosRecomp declares these inputs uint4: blend indices are converted to float by the shader, the normal frame goes through asfloat().
inline bool DeclUsageIsInteger(uint32_t usage) {
  return usage == kUsageBlendIndices || usage == kUsageNormal || usage == kUsageTangent || usage == kUsageBinormal;
}

// The four values a shader input receives for one element: the fields of each dword as the fetch constant's endian swap leaves them, then the declaration's selectors.
void DecodeVertexElement(const GuestVertexType& type, const uint8_t* src, uint32_t endian, float out[4]);

}  // namespace cherry::gpu
