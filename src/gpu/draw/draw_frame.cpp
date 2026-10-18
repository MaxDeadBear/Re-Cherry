#include "gpu/draw/draw_frame.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <unordered_map>

#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/debug/bad_vertices.h"
#include "gpu/debug/frame_timing.h"
#include "gpu/debug/late_vertex_writes.h"
#include "gpu/draw/vertex_format.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"
#include "gpu/guest_writes.h"
#include "gpu/shaders/shader_cache.h"
#include "gpu/shaders/shader_registry.h"

namespace cherry::gpu {

namespace {

// The guest vertex declaration built by XGSetVertexDeclarationHeader: element count at +24, 12-byte D3DVERTEXELEMENT9s from +52.
constexpr uint32_t kDeclElementCount = 24;
constexpr uint32_t kDeclElements = 52;
constexpr uint32_t kDeclElementSize = 12;
constexpr uint32_t kMaxDeclElements = 32;
// D3DIndexBuffer (XGSetIndexBufferHeader): Common's sign bit selects 32-bit indices, bits 29..30 the endian swap; address at +0x18, length in bytes at +0x1C.
constexpr uint32_t kIndexBufferAddress = 0x18;
constexpr uint32_t kIndexBufferLength = 0x1C;
// D3DSurface size word: width - 1 in bits 18..31, height - 1 in bits 3..17 (as D3DDevice_ClearF reads it).
constexpr uint32_t kSurfaceSize = 0x24;
// D3DSurface: SurfaceInfo at +0x18, Color0Info or DepthInfo at +0x1C.
constexpr uint32_t kSurfaceSurfaceInfo = 0x18;
constexpr uint32_t kSurfaceInfo = 0x1C;
// D3DBaseTexture: the six fetch-constant dwords from +0x1C.
constexpr uint32_t kTextureFetchWords = 0x1C;
constexpr uint32_t kMaxVerticesPerDraw = 1u << 20;

enum GuestPrimitive : uint32_t {
  kPointList = 1,
  kLineList = 2,
  kLineStrip = 3,
  kTriangleList = 4,
  kTriangleFan = 5,
  kTriangleStrip = 6,
  kRectList = 8,
  kQuadList = 13,
};

// Where one element is fetched from: stream n's vertex fetch constant, 95 - n.
struct GuestElement {
  uint32_t fetch_slot = 0;
  uint32_t stream = 0;
  uint32_t offset = 0;
  GuestVertexType type;
};

struct DeclInfo {
  HostLayout layout;
  std::vector<GuestElement> guest;
  bool valid = false;
};

// A Begin*Vertices draw: its state is captured at the call, its data once the guest has written it.
struct PendingDraw {
  size_t record = 0;
  const DeclInfo* decl = nullptr;
  uint32_t vertex_data = 0;
  uint32_t vertex_count = 0;
  uint32_t stride = 0;
  uint32_t index_data = 0;
  uint32_t index_count = 0;
  bool index_32bit = false;
  uint32_t index_endian = 0;
  int32_t base_vertex = 0;  // added to every index, as BeginIndexedVertices sets VGT_INDX_OFFSET
  uint32_t primitive = 0;
};

std::mutex g_mutex;
DrawFrame g_frame;
std::vector<PendingDraw> g_pending;
// Keyed by address and checked against the contents, since the guest recycles a declaration's memory; superseded entries stay alive for draws that point at them.
std::deque<DeclInfo> g_decl_storage;
std::map<std::pair<uint32_t, uint64_t>, DeclInfo*> g_decls;
// Converted geometry, keyed by what its conversion depends on besides the guest bytes, which `content` hashes; an entry whose bytes change stops being cached.
struct GeometryEntry {
  uint64_t content = 0;
  // The physical ranges `content` covers, and the write-watch stamp they were last checked under; empty when one lies outside the watchable views.
  std::vector<std::pair<uint32_t, uint32_t>> watched;
  uint64_t watch_stamp = 0;
  uint32_t first = 0;
  uint32_t last = 0;
  uint64_t used_frame = 0;
  bool dynamic = false;
  std::shared_ptr<HostGeometry> geometry;
};
std::unordered_map<uint64_t, GeometryEntry> g_geometry;
uint64_t g_capture_frame = 0;
constexpr uint64_t kGeometryUnusedFrames = 600;

const uint8_t* GuestVirtual(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

const uint8_t* GuestPhysical(uint8_t* base, uint32_t physical) {
  return base + 0xA0000000u + (physical & 0x1FFFFFFFu);
}

uint64_t HashBytes(const uint8_t* data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
  return hash;
}

const DeclInfo* GetDecl(uint8_t* base, uint32_t decl) {
  if (!decl) return nullptr;
  const uint32_t count = REX_LOAD_U32(decl + kDeclElementCount);
  if (count == 0 || count > kMaxDeclElements) return nullptr;
  const uint64_t signature = HashBytes(GuestVirtual(base, decl + kDeclElements), size_t(count) * kDeclElementSize);
  auto found = g_decls.find({decl, signature});
  if (found != g_decls.end()) return found->second->valid ? found->second : nullptr;

  DeclInfo& info = g_decl_storage.emplace_back();
  g_decls.emplace(std::pair{decl, signature}, &info);
  uint32_t fallback_location = 23;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t element = decl + kDeclElements + i * kDeclElementSize;
    const uint32_t stream = REX_LOAD_U16(element);
    if (stream == 0xFF) break;
    GuestElement guest;
    guest.stream = stream;
    guest.fetch_slot = 95 - stream;
    guest.offset = REX_LOAD_U16(element + 2);
    const uint32_t xg_type = REX_LOAD_U32(element + 4);
    guest.type = DecodeVertexType(xg_type);
    if (guest.type.size == 0 || stream >= 16) {
      REXGPU_WARN("[gfx] vertex declaration {:08X} element {} has unhandled type {:08X} on stream {}", decl, i, xg_type, stream);
      return nullptr;
    }
    HostElement host;
    host.usage = REX_LOAD_U8(element + 9);
    host.usage_index = REX_LOAD_U8(element + 10);
    host.integer = DeclUsageIsInteger(host.usage);
    host.guest_type = xg_type;
    host.guest_offset = guest.offset;
    host.guest_stream = stream;
    if (!DeclUsageLocation(host.usage, host.usage_index, &host.location)) host.location = fallback_location++;
    info.guest.push_back(guest);
    info.layout.elements.push_back(host);
  }
  info.layout.key = signature;
  info.valid = !info.layout.elements.empty();
  return info.valid ? &info : nullptr;
}

// `streams`, `strides` and `sizes` are per element: each element's buffer start, stride and readable bytes.
void WriteVertex(const DeclInfo& decl, const uint8_t* const* streams, const uint32_t* strides, const uint32_t* sizes, const uint32_t* endians, uint32_t vertex, uint8_t* out) {
  for (size_t e = 0; e < decl.guest.size(); ++e) {
    const GuestElement& guest = decl.guest[e];
    float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const uint64_t at = uint64_t(vertex) * strides[e] + guest.offset;
    if (streams[e] && at + guest.type.size <= sizes[e]) DecodeVertexElement(guest.type, streams[e] + at, endians[e], values);
    uint32_t words[4];
    const HostElement& host = decl.layout.elements[e];
    for (int c = 0; c < 4; ++c) {
      // Blend indices are converted to float by the shader; the normal frame is asfloat()'d.
      words[c] = host.integer && host.usage == kUsageBlendIndices ? uint32_t(std::max(values[c], 0.0f)) : std::bit_cast<uint32_t>(values[c]);
    }
    std::memcpy(out + e * 16, words, 16);
  }
}

uint32_t ReadIndex(const uint8_t* data, uint32_t i, bool index_32bit, uint32_t endian) {
  if (index_32bit) {
    uint32_t v;
    std::memcpy(&v, data + 4 * i, 4);
    if (endian == 2) return std::byteswap(v);
    if (endian == 3) return (v >> 16) | (v << 16);
    if (endian == 1) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return v;
  }
  uint16_t v;
  std::memcpy(&v, data + 2 * i, 2);
  return endian == 1 || endian == 2 ? std::byteswap(v) : v;
}

// Turns the guest primitive into one the host draws, adding vertices for rect lists; false when the primitive has no host form.
bool ExpandPrimitive(uint32_t primitive, std::vector<uint32_t>& indices, std::vector<uint8_t>& vertices, uint32_t stride, DrawTopology* topology) {
  switch (primitive) {
    case kPointList: *topology = DrawTopology::PointList; return true;
    case kLineList: *topology = DrawTopology::LineList; return true;
    case kLineStrip: *topology = DrawTopology::LineStrip; return true;
    case kTriangleList: *topology = DrawTopology::TriangleList; return true;
    case kTriangleStrip: *topology = DrawTopology::TriangleStrip; return true;
    case kTriangleFan: {
      std::vector<uint32_t> list;
      for (size_t i = 1; i + 1 < indices.size(); ++i) list.insert(list.end(), {indices[0], indices[i], indices[i + 1]});
      indices.swap(list);
      *topology = DrawTopology::TriangleList;
      return true;
    }
    case kQuadList: {
      std::vector<uint32_t> list;
      for (size_t i = 0; i + 3 < indices.size(); i += 4) list.insert(list.end(), {indices[i], indices[i + 1], indices[i + 2], indices[i], indices[i + 2], indices[i + 3]});
      indices.swap(list);
      *topology = DrawTopology::TriangleList;
      return true;
    }
    case kRectList: {
      // Each rectangle is three corners; UE3 (sub_8297EBB0) draws the unit square as (0,0), (1,0), (0,1), so the fourth is v1 + v2 - v0.
      std::vector<uint32_t> list;
      const uint32_t words = stride / 4;
      for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t v0 = indices[i], v1 = indices[i + 1], v2 = indices[i + 2];
        const uint32_t v3 = uint32_t(vertices.size() / stride);
        vertices.resize(vertices.size() + stride);
        const float* a = reinterpret_cast<const float*>(vertices.data() + size_t(v0) * stride);
        const float* b = reinterpret_cast<const float*>(vertices.data() + size_t(v1) * stride);
        const float* c = reinterpret_cast<const float*>(vertices.data() + size_t(v2) * stride);
        float* d = reinterpret_cast<float*>(vertices.data() + size_t(v3) * stride);
        for (uint32_t w = 0; w < words; ++w) d[w] = b[w] + c[w] - a[w];
        list.insert(list.end(), {v0, v1, v2, v2, v1, v3});
      }
      indices.swap(list);
      *topology = DrawTopology::TriangleList;
      return true;
    }
    default:
      return false;
  }
}

SurfaceBinding ReadSurface(uint8_t* base, uint32_t surface) {
  SurfaceBinding binding;
  if (!surface) return binding;
  const uint32_t size = REX_LOAD_U32(surface + kSurfaceSize);
  binding.info = REX_LOAD_U32(surface + kSurfaceInfo);
  binding.surface_info = REX_LOAD_U32(surface + kSurfaceSurfaceInfo);
  binding.width = (size >> 18) + 1;
  binding.height = ((size >> 3) & 0x7FFF) + 1;
  return binding;
}

void CaptureTargets(uint8_t* base, uint32_t device, DrawRecord& record) {
  record.color = ReadSurface(base, REX_LOAD_U32(device + dev::kRenderTarget0));
  for (uint32_t i = 0; i < 3; ++i) record.extra_colors[i] = ReadSurface(base, REX_LOAD_U32(device + dev::kRenderTarget0 + 4 * (i + 1)));
  record.depth = ReadSurface(base, REX_LOAD_U32(device + dev::kDepthStencil));
}

void ReadFloats(uint8_t* base, uint32_t address, float* out, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) out[i] = address ? std::bit_cast<float>(REX_LOAD_U32(address + 4 * i)) : 0.0f;
}

// Fills everything but the geometry from the device's shadow state; false when the draw cannot be replayed.
// `strides` receives each stream's stride in dwords: the device's from SetStreamSource, or the ones D3DVertexShader_Bind was given when the declaration came from the bound vertex shader.
bool CaptureState(uint8_t* base, uint32_t device, DrawRecord& record, const DeclInfo** decl, uint8_t strides[16]) {
  record.vs = LookupShaderBinding(REX_LOAD_U32(device + dev::kVertexShader));
  record.ps = LookupShaderBinding(REX_LOAD_U32(device + dev::kPixelShader));
  // No pixel shader bound is a depth-only draw (shadow and depth pre-passes); an unknown one cannot be replayed.
  if (!record.vs || (!record.ps && REX_LOAD_U32(device + dev::kPixelShader))) {
    ++g_frame.dropped_no_shader;
    static uint32_t reported = 0;
    if (reported++ < 6) REXGPU_WARN("[gfx] draw without a translated shader: VS object {:08X} ({}), PS object {:08X} ({})", REX_LOAD_U32(device + dev::kVertexShader), record.vs ? "bound" : "unknown", REX_LOAD_U32(device + dev::kPixelShader), record.ps ? "bound" : "unknown");
    return false;
  }
  uint32_t decl_address = REX_LOAD_U32(device + dev::kVertexDeclaration);
  for (uint32_t s = 0; s < 16; ++s) strides[s] = REX_LOAD_U8(device + dev::kStreamStrides + s);
  // UE3 binds its declarations through D3DVertexShader_Bind and leaves the device's unset.
  if (VertexShaderBinding binding; !decl_address && LookupVertexShaderBinding(REX_LOAD_U32(device + dev::kVertexShader), &binding)) {
    decl_address = binding.declaration;
    std::memcpy(strides, binding.strides, sizeof(binding.strides));
  }
  *decl = decl_address ? GetDecl(base, decl_address) : nullptr;
  if (!*decl) {
    ++g_frame.dropped_no_layout;
    static uint32_t reported = 0;
    if (reported++ < 6) REXGPU_WARN("[gfx] draw without a usable vertex declaration: decl {:08X}, words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", decl_address, decl_address ? REX_LOAD_U32(decl_address) : 0, decl_address ? REX_LOAD_U32(decl_address + 4) : 0, decl_address ? REX_LOAD_U32(decl_address + 24) : 0, decl_address ? REX_LOAD_U32(decl_address + 28) : 0, decl_address ? REX_LOAD_U32(decl_address + 52) : 0, decl_address ? REX_LOAD_U32(decl_address + 56) : 0, decl_address ? REX_LOAD_U32(decl_address + 60) : 0);
    return false;
  }
  record.layout = &(*decl)->layout;

  // Only the registers each shader reads, byte-swapped through plain pointers, which the compiler vectorises; REX_LOAD_U32's volatile loads go a word at a time.
  const uint32_t vs_words = std::min(VertexConstantRegisters(record), 256u) * 4;
  const uint32_t ps_words = std::min(PixelConstantRegisters(record), 256u) * 4;
  record.constants = g_frame.Alloc(size_t(vs_words + ps_words) * 4);
  uint32_t* constants = reinterpret_cast<uint32_t*>(g_frame.arena.data() + record.constants);
  const uint32_t* vertex_constants = reinterpret_cast<const uint32_t*>(GuestVirtual(base, device + dev::kVertexShaderConstants));
  const uint32_t* pixel_constants = reinterpret_cast<const uint32_t*>(GuestVirtual(base, device + dev::kPixelShaderConstants));
  for (uint32_t i = 0; i < vs_words; ++i) constants[i] = std::byteswap(vertex_constants[i]);
  for (uint32_t i = 0; i < ps_words; ++i) constants[vs_words + i] = std::byteswap(pixel_constants[i]);
  // Vertex bools b0..b15 are bits 0..15; pixel bools (b128 + N in the microcode) are bits 16..31.
  record.booleans = (REX_LOAD_U32(device + dev::kBoolConstants) & 0xFFFF) | ((REX_LOAD_U32(device + dev::kBoolConstants + 16) & 0xFFFF) << 16);
  for (uint32_t s = 0; s < kSamplerSlots; ++s) {
    if (!REX_LOAD_U32(device + dev::kTextureObjects + 4 * s)) continue;
    for (uint32_t w = 0; w < 6; ++w) record.textures[s].words[w] = REX_LOAD_U32(device + dev::kFetchConstants + 24 * s + 4 * w);
    if (record.textures[s].valid()) record.texture_mask |= 1u << s;
  }
  CaptureTargets(base, device, record);
  record.color_mask = REX_LOAD_U32(device + dev::kColorMask) & 0xFFFF;
  record.color_control = REX_LOAD_U32(device + dev::kColorControl);
  record.mode_control = REX_LOAD_U32(device + dev::kModeControl);
  if (record.mode_control & 0x1800) {
    record.poly_offset_scale = std::bit_cast<float>(REX_LOAD_U32(device + dev::kPolyOffsetFrontScale));
    record.poly_offset = std::bit_cast<float>(REX_LOAD_U32(device + dev::kPolyOffsetFrontOffset));
  }
  record.alpha_ref = std::bit_cast<float>(REX_LOAD_U32(device + dev::kAlphaRef));
  record.stencil_ref_mask = REX_LOAD_U32(device + dev::kStencilRefMask);
  const uint32_t scissor_tl = REX_LOAD_U32(device + dev::kWindowScissorTL);
  const uint32_t scissor_br = REX_LOAD_U32(device + dev::kWindowScissorBR);
  record.scissor[0] = int32_t(scissor_tl & 0x7FFF);
  record.scissor[1] = int32_t((scissor_tl >> 16) & 0x7FFF);
  record.scissor[2] = int32_t(scissor_br & 0x7FFF);
  record.scissor[3] = int32_t((scissor_br >> 16) & 0x7FFF);
  record.blend_control = REX_LOAD_U32(device + dev::kBlendControl0);
  for (uint32_t i = 0; i < 3; ++i) record.extra_blend_controls[i] = REX_LOAD_U32(device + dev::kBlendControl1 + 4 * i);
  record.depth_control = REX_LOAD_U32(device + dev::kDepthControl);
  for (int i = 0; i < 6; ++i) record.viewport[i] = std::bit_cast<float>(REX_LOAD_U32(device + dev::kViewportScaleOffset + 4 * i));
  record.vte_control = REX_LOAD_U32(device + dev::kVteControl);
  record.vtx_control = REX_LOAD_U32(device + dev::kVtxControl);
  if (const uint32_t target = REX_LOAD_U32(device + dev::kRenderTarget0)) {
    const uint32_t size = REX_LOAD_U32(target + kSurfaceSize);
    record.target_width = (size >> 18) + 1;
    record.target_height = ((size >> 3) & 0x7FFF) + 1;
  }
  return true;
}

void StoreGeometry(DrawRecord& record, std::vector<uint8_t>& vertices, std::vector<uint32_t>& indices, uint32_t primitive) {
  if (!ExpandPrimitive(primitive, indices, vertices, record.layout->stride(), &record.topology) || indices.empty()) {
    ++g_frame.dropped_other;
    record.index_count = 0;
    return;
  }
  record.vertex_count = uint32_t(vertices.size() / record.layout->stride());
  record.vertices = g_frame.Alloc(vertices.size());
  std::memcpy(g_frame.arena.data() + record.vertices, vertices.data(), vertices.size());
  record.index_count = uint32_t(indices.size());
  record.indices = g_frame.Alloc(indices.size() * 4);
  std::memcpy(g_frame.arena.data() + record.indices, indices.data(), indices.size() * 4);
}

// The guest bytes a draw reads: its indices, then each stream's vertices first..last.
uint64_t HashDrawContent(const uint8_t* index_data, uint32_t index_bytes, const std::vector<const uint8_t*>& streams, const std::vector<uint32_t>& strides, const std::vector<uint32_t>& sizes, uint32_t first, uint32_t last) {
  uint64_t hash = index_data ? XXH3_64bits(index_data, index_bytes) : 0;
  for (size_t e = 0; e < streams.size(); ++e) {
    if (!streams[e]) continue;
    bool seen = false;
    for (size_t p = 0; p < e && !seen; ++p) seen = streams[p] == streams[e] && strides[p] == strides[e];
    if (seen) continue;
    const uint64_t begin = std::min<uint64_t>(uint64_t(first) * strides[e], sizes[e]);
    const uint64_t end = std::min<uint64_t>((uint64_t(last) + 1) * strides[e], sizes[e]);
    hash = XXH3_64bits_withSeed(streams[e] + begin, size_t(end - begin), hash);
  }
  return hash;
}

void NoteDrawBytes(uint8_t* base, const DrawRecord& record, const uint8_t* index_data, uint32_t index_bytes, const std::vector<const uint8_t*>& streams, const std::vector<uint32_t>& strides, const std::vector<uint32_t>& sizes, uint32_t first, uint32_t last) {
  std::vector<std::pair<const uint8_t*, size_t>> ranges;
  if (index_data) ranges.emplace_back(index_data, index_bytes);
  for (size_t e = 0; e < streams.size(); ++e) {
    if (!streams[e]) continue;
    const uint64_t begin = std::min<uint64_t>(uint64_t(first) * strides[e], sizes[e]);
    const uint64_t end = std::min<uint64_t>((uint64_t(last) + 1) * strides[e], sizes[e]);
    ranges.emplace_back(streams[e] + begin, size_t(end - begin));
  }
  const uint32_t address = streams.empty() || !streams[0] ? 0 : uint32_t(streams[0] - (base + 0xA0000000u));
  debug::NoteDrawBytes(record.vs->hash, address, ranges);
}

std::string DescribeDecl(const DeclInfo& decl) {
  std::string text;
  for (size_t e = 0; e < decl.guest.size(); ++e) text += std::format(" [{} {} stream {} offset {} type {}/{}]", DeclUsageSemantic(decl.layout.elements[e].usage), decl.layout.elements[e].usage_index, decl.guest[e].stream, decl.guest[e].offset, decl.guest[e].type.data_format, decl.guest[e].type.num_format);
  return text;
}

void UseGeometry(DrawRecord& record, std::shared_ptr<HostGeometry> geometry) {
  record.topology = geometry->topology;
  record.vertex_count = geometry->vertex_count;
  record.index_count = geometry->index_count;
  record.geometry = std::move(geometry);
}

// Vertices per primitive of a list, whose primitives an out-of-range index drops whole; 0 for strips and fans.
uint32_t ListPrimitiveSize(uint32_t primitive) {
  switch (primitive) {
    case kPointList: return 1;
    case kLineList: return 2;
    case kTriangleList:
    case kRectList: return 3;
    case kQuadList: return 4;
    default: return 0;
  }
}

// Reads a Begin*Vertices or DrawVerticesUP draw's data once the guest has written it.
void ReadPending(uint8_t* base, const PendingDraw& pending) {
  {
    DrawRecord& record = g_frame.draws[pending.record];
    const size_t elements = pending.decl->guest.size();
    const std::vector<const uint8_t*> streams(elements, GuestVirtual(base, pending.vertex_data));
    const std::vector<uint32_t> strides(elements, pending.stride);
    const std::vector<uint32_t> sizes(elements, pending.vertex_count * pending.stride);
    // D3D's own fetch constants for Begin*Vertices data swap 8-in-32.
    const std::vector<uint32_t> endians(elements, 2u);
    const uint32_t host_stride = record.layout->stride();
    std::vector<uint8_t> vertices(size_t(pending.vertex_count) * host_stride);
    for (uint32_t v = 0; v < pending.vertex_count; ++v) WriteVertex(*pending.decl, streams.data(), strides.data(), sizes.data(), endians.data(), v, vertices.data() + size_t(v) * host_stride);
    debug::CheckVertices(record.layout[0], vertices.data(), pending.vertex_count, [&] { return std::format("Begin*Vertices VS {:016X} data {:08X} count {} stride {} indices {:08X} x{} base {} primitive {}{}", record.vs->hash, pending.vertex_data, pending.vertex_count, pending.stride, pending.index_data, pending.index_count, pending.base_vertex, pending.primitive, DescribeDecl(*pending.decl)); });
    std::vector<uint32_t> indices;
    if (pending.index_data) {
      const uint8_t* data = GuestVirtual(base, pending.index_data);
      indices.resize(pending.index_count);
      for (uint32_t i = 0; i < pending.index_count; ++i) indices[i] = uint32_t(int32_t(ReadIndex(data, i, pending.index_32bit, pending.index_endian)) + pending.base_vertex);
      const uint32_t size = ListPrimitiveSize(pending.primitive);
      if (size) {
        std::vector<uint32_t> kept;
        kept.reserve(indices.size());
        for (size_t i = 0; i + size <= indices.size(); i += size) {
          if (std::all_of(indices.begin() + i, indices.begin() + i + size, [&](uint32_t index) { return index < pending.vertex_count; })) kept.insert(kept.end(), indices.begin() + i, indices.begin() + i + size);
        }
        indices.swap(kept);
      } else {
        for (uint32_t& index : indices) index = std::min(index, pending.vertex_count - 1);
      }
    } else {
      for (uint32_t v = 0; v < pending.vertex_count; ++v) indices.push_back(v);
    }
    StoreGeometry(record, vertices, indices, pending.primitive);
  }
}

// Begin*Vertices data comes from the per-frame data sink, which is only reset after the frame is taken, so it is read then, as the GPU would.
void FlushPending(uint8_t* base) {
  for (const PendingDraw& pending : g_pending) ReadPending(base, pending);
  g_pending.clear();
}

}  // namespace

uint32_t DrawFrame::Alloc(size_t bytes) {
  const size_t offset = (arena.size() + 15) & ~size_t(15);
  arena.resize(offset + bytes);
  return uint32_t(offset);
}

void DrawFrame::Clear() {
  arena.clear();
  draws.clear();
  dropped_no_shader = 0;
  dropped_no_layout = 0;
  dropped_other = 0;
}

// Arms write watches over the bytes an entry's conversion read, before they are hashed; gives up on watching when one range is outside the watchable views.
void WatchDrawBytes(GeometryEntry& entry, uint32_t index_address, uint32_t index_bytes, bool indexed, uint32_t start, bool index_32bit, const std::vector<uint32_t>& stream_addresses, const std::vector<uint32_t>& strides, const std::vector<uint32_t>& sizes) {
  entry.watched.clear();
  entry.watch_stamp = 0;
  if (indexed) {
    const uint32_t physical = WatchablePhysical(index_address);
    if (physical == ~0u) return;
    entry.watched.emplace_back(physical + start * (index_32bit ? 4u : 2u), index_bytes);
  }
  for (size_t e = 0; e < stream_addresses.size(); ++e) {
    if (!stream_addresses[e]) continue;
    const uint64_t begin = std::min<uint64_t>(uint64_t(entry.first) * strides[e], sizes[e]);
    const uint64_t end = std::min<uint64_t>((uint64_t(entry.last) + 1) * strides[e], sizes[e]);
    if (end > begin) entry.watched.emplace_back(stream_addresses[e] + uint32_t(begin), uint32_t(end - begin));
  }
  uint64_t stamp = 0;
  for (const auto& [physical, length] : entry.watched) {
    const uint64_t armed = ArmWriteWatch(physical, length);
    if (!armed) {
      entry.watched.clear();
      return;
    }
    if (!stamp) stamp = armed;
  }
  entry.watch_stamp = stamp;
}

void CaptureDraw(uint32_t device, uint32_t primitive_type, uint32_t start, uint32_t count, bool indexed) {
  debug::ScopedFrameTimer timer(debug::TimingPhase::Capture);
  if (!device || count == 0) return;
  uint8_t* base = GuestBase();
  std::lock_guard<std::mutex> lock(g_mutex);
  DrawRecord record;
  const DeclInfo* decl = nullptr;
  uint8_t stream_strides[16] = {};
  if (!CaptureState(base, device, record, &decl, stream_strides)) return;

  const size_t elements = decl->guest.size();
  std::vector<const uint8_t*> streams(elements);
  std::vector<uint32_t> stream_addresses(elements);
  std::vector<uint32_t> strides(elements);
  std::vector<uint32_t> sizes(elements);
  std::vector<uint32_t> endians(elements);
  for (size_t e = 0; e < elements; ++e) {
    const GuestElement& element = decl->guest[e];
    const uint32_t fetch = device + dev::kFetchConstants + 8 * element.fetch_slot;
    const uint32_t fetch0 = REX_LOAD_U32(fetch);
    const uint32_t fetch1 = REX_LOAD_U32(fetch + 4);
    endians[e] = fetch1 & 3;
    stream_addresses[e] = fetch0 & ~3u;
    streams[e] = (fetch0 & ~3u) ? GuestPhysical(base, fetch0 & ~3u) : nullptr;
    sizes[e] = fetch1 & 0x03FFFFFCu;
    strides[e] = uint32_t(stream_strides[element.stream]) << 2;
  }

  const uint8_t* index_data = nullptr;
  bool index_32bit = false;
  uint32_t index_endian = 0;
  uint32_t index_address = 0;
  if (indexed) {
    const uint32_t index_buffer = REX_LOAD_U32(device + dev::kIndexBuffer);
    if (!index_buffer) {
      ++g_frame.dropped_other;
      return;
    }
    const uint32_t common = REX_LOAD_U32(index_buffer);
    index_32bit = (common & 0x80000000u) != 0;
    index_endian = (common >> 29) & 3;
    const uint32_t length = REX_LOAD_U32(index_buffer + kIndexBufferLength);
    if (uint64_t(start + count) * (index_32bit ? 4 : 2) > length) {
      ++g_frame.dropped_other;
      return;
    }
    index_address = REX_LOAD_U32(index_buffer + kIndexBufferAddress);
    index_data = GuestVirtual(base, index_address) + size_t(start) * (index_32bit ? 4 : 2);
  }
  const uint32_t index_bytes = indexed ? count * (index_32bit ? 4 : 2) : 0;

  uint64_t key_words[8 + 4 * kMaxDeclElements];
  size_t key_size = 0;
  key_words[key_size++] = reinterpret_cast<uintptr_t>(decl);
  key_words[key_size++] = (uint64_t(primitive_type) << 32) | (uint64_t(indexed) << 1) | uint64_t(index_32bit);
  key_words[key_size++] = (uint64_t(start) << 32) | count;
  key_words[key_size++] = (uint64_t(index_address) << 32) | index_endian;
  for (size_t e = 0; e < elements; ++e) {
    key_words[key_size++] = reinterpret_cast<uintptr_t>(streams[e]);
    key_words[key_size++] = (uint64_t(strides[e]) << 32) | sizes[e];
    key_words[key_size++] = endians[e];
  }
  GeometryEntry& entry = g_geometry[XXH3_64bits(key_words, key_size * sizeof(uint64_t))];
  entry.used_frame = g_capture_frame;
  // The vertex atlas is filled from the arena.
  if (record.vs->specConstantsMask & kShaderIndexedVertexFetch) {
    entry.dynamic = true;
    entry.geometry.reset();
  }
  if (entry.geometry) {
    if (debug::LateVertexWritesEnabled()) NoteDrawBytes(base, record, index_data, index_bytes, streams, strides, sizes, entry.first, entry.last);
    // Unwritten since the last check, or the same bytes: the cached conversion stands; the hash also covers ranges no watch reaches.
    const bool unwritten = entry.watch_stamp && std::none_of(entry.watched.begin(), entry.watched.end(), [&](const auto& range) { return WrittenSince(range.first, range.second, entry.watch_stamp); });
    if (unwritten || HashDrawContent(index_data, index_bytes, streams, strides, sizes, entry.first, entry.last) == entry.content) {
      // Written with the same bytes: the guest refills it every frame, so it is hashed from now on rather than faulting a watch each time.
      if (!unwritten && entry.watch_stamp) {
        entry.watched.clear();
        entry.watch_stamp = 0;
      }
      record.vertex_index_base = int32_t(entry.first);
      UseGeometry(record, entry.geometry);
      g_frame.draws.push_back(std::move(record));
      return;
    }
    entry.dynamic = true;
    entry.geometry.reset();
  }

  std::vector<uint32_t> guest_indices(count);
  if (indexed) {
    for (uint32_t i = 0; i < count; ++i) guest_indices[i] = ReadIndex(index_data, i, index_32bit, index_endian);
  } else {
    for (uint32_t i = 0; i < count; ++i) guest_indices[i] = start + i;
  }

  // Only the vertex range the draw reads is copied, rebased to zero.
  const uint32_t first = *std::min_element(guest_indices.begin(), guest_indices.end());
  const uint32_t last = *std::max_element(guest_indices.begin(), guest_indices.end());
  if (last - first >= kMaxVerticesPerDraw) {
    ++g_frame.dropped_other;
    return;
  }
  if (debug::LateVertexWritesEnabled() && !entry.geometry) NoteDrawBytes(base, record, index_data, index_bytes, streams, strides, sizes, first, last);
  const uint32_t host_stride = decl->layout.stride();
  std::vector<uint8_t> vertices(size_t(last - first + 1) * host_stride);
  for (uint32_t v = first; v <= last; ++v) WriteVertex(*decl, streams.data(), strides.data(), sizes.data(), endians.data(), v, vertices.data() + size_t(v - first) * host_stride);
  debug::CheckVertices(decl->layout, vertices.data(), last - first + 1, [&] {
    std::string text = std::format("Draw{} VS {:016X} start {} count {} vertices {}..{} primitive {}{}", indexed ? "Indexed" : "", record.vs->hash, start, count, first, last, primitive_type, DescribeDecl(*decl));
    for (size_t e = 0; e < elements; ++e) text += std::format(" | e{} slot {} fetch {:08X} {:08X} stride {}", e, decl->guest[e].fetch_slot, REX_LOAD_U32(device + dev::kFetchConstants + 8 * decl->guest[e].fetch_slot), REX_LOAD_U32(device + dev::kFetchConstants + 8 * decl->guest[e].fetch_slot + 4), strides[e]);
    return text;
  });
  for (uint32_t& index : guest_indices) index -= first;
  record.vertex_index_base = int32_t(first);
  if (entry.dynamic) {
    StoreGeometry(record, vertices, guest_indices, primitive_type);
    if (record.index_count) g_frame.draws.push_back(std::move(record));
    return;
  }
  auto geometry = std::make_shared<HostGeometry>();
  if (!ExpandPrimitive(primitive_type, guest_indices, vertices, host_stride, &geometry->topology) || guest_indices.empty()) {
    ++g_frame.dropped_other;
    return;
  }
  geometry->vertex_count = uint32_t(vertices.size() / host_stride);
  geometry->index_count = uint32_t(guest_indices.size());
  geometry->vertices = std::move(vertices);
  geometry->indices = std::move(guest_indices);
  entry.first = first;
  entry.last = last;
  WatchDrawBytes(entry, index_address, index_bytes, indexed, start, index_32bit, stream_addresses, strides, sizes);
  entry.content = HashDrawContent(index_data, index_bytes, streams, strides, sizes, first, last);
  entry.geometry = geometry;
  UseGeometry(record, std::move(geometry));
  g_frame.draws.push_back(std::move(record));
}

void CaptureDrawFromMemory(uint32_t device, uint32_t primitive_type, uint32_t vertex_data, uint32_t vertex_count, uint32_t stride, uint32_t index_data, uint32_t index_count, uint32_t index_format, int32_t base_vertex, bool deferred) {
  debug::ScopedFrameTimer timer(debug::TimingPhase::Capture);
  if (!device || !vertex_data || vertex_count == 0 || vertex_count > kMaxVerticesPerDraw) return;
  uint8_t* base = GuestBase();
  std::lock_guard<std::mutex> lock(g_mutex);
  DrawRecord record;
  const DeclInfo* decl = nullptr;
  uint8_t stream_strides[16] = {};
  if (!CaptureState(base, device, record, &decl, stream_strides)) return;
  g_frame.draws.push_back(record);
  // D3DFORMAT index formats: bit 2 selects 32-bit indices, bits 0..1 are the endian swap BeginIndexedVertices puts in the draw packet.
  const PendingDraw pending = {g_frame.draws.size() - 1, decl, vertex_data, vertex_count, stride, index_data, index_count, (index_format & 4) != 0, index_format & 3, base_vertex, primitive_type};
  if (deferred) g_pending.push_back(pending);
  else ReadPending(base, pending);
}

void CaptureClear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, float depth) {
  if (!device) return;
  uint8_t* base = GuestBase();
  std::lock_guard<std::mutex> lock(g_mutex);
  DrawRecord record;
  record.kind = CommandKind::Clear;
  CaptureTargets(base, device, record);
  record.clear_flags = flags;
  if (rect) {
    record.clear_has_rect = true;
    for (int i = 0; i < 4; ++i) record.clear_rect[i] = int32_t(REX_LOAD_U32(rect + 4 * i));
  }
  ReadFloats(base, color, record.clear_color, 4);
  record.clear_depth = depth;
  g_frame.draws.push_back(record);
}

void CaptureResolve(uint32_t device, uint32_t flags, uint32_t source_rect, uint32_t dest_texture, uint32_t dest_point, uint32_t dest_slice, uint32_t clear_color, float clear_depth) {
  if (!device || !dest_texture) return;
  uint8_t* base = GuestBase();
  std::lock_guard<std::mutex> lock(g_mutex);
  DrawRecord record;
  record.kind = CommandKind::Resolve;
  CaptureTargets(base, device, record);
  record.resolve_flags = flags;
  const uint32_t source = flags & 7;
  record.resolve_source = source == 4 ? record.depth : ReadSurface(base, REX_LOAD_U32(device + dev::kRenderTarget0 + 4 * std::min(source, 3u)));
  if (source_rect) {
    for (int i = 0; i < 4; ++i) record.resolve_rect[i] = int32_t(REX_LOAD_U32(source_rect + 4 * i));
  } else {
    record.resolve_rect[2] = int32_t(record.resolve_source.width);
    record.resolve_rect[3] = int32_t(record.resolve_source.height);
  }
  for (uint32_t w = 0; w < 6; ++w) record.resolve_dest.words[w] = REX_LOAD_U32(dest_texture + kTextureFetchWords + 4 * w);
  if (dest_point) {
    record.resolve_point[0] = int32_t(REX_LOAD_U32(dest_point));
    record.resolve_point[1] = int32_t(REX_LOAD_U32(dest_point + 4));
  }
  record.resolve_slice = dest_slice;
  ReadFloats(base, clear_color, record.resolve_clear_color, 4);
  record.resolve_clear_depth = clear_depth;
  g_frame.draws.push_back(record);
}

void TakeCapturedFrame(DrawFrame& out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  FlushPending(GuestBase());
  std::erase_if(g_frame.draws, [](const DrawRecord& record) { return record.kind == CommandKind::Draw && record.index_count == 0; });
  debug::CheckDrawBytes();
  std::swap(out, g_frame);
  g_frame.Clear();
  if ((++g_capture_frame % 300) == 0) std::erase_if(g_geometry, [](const auto& item) { return item.second.used_frame + kGeometryUnusedFrames < g_capture_frame; });
}

}  // namespace cherry::gpu
