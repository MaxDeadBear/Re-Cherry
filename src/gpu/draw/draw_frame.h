// The capture/replay contract: the D3D draw hooks record each draw from the guest device's shadow state, and the present replays the frame through plume.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "gpu/shaders/shader_cache.h"
#include "gpu/textures/texture_cache.h"

struct ShaderCacheEntry;

namespace plume {
struct RenderBuffer;
}  // namespace plume

namespace cherry::gpu {

// One host input element: every guest element is decoded to four 32-bit values, float or (for the shader's uint4 inputs) raw bits.
struct HostElement {
  uint32_t usage = 0;
  uint32_t usage_index = 0;
  uint32_t location = 0;
  uint32_t guest_type = 0;  // the declaration's D3DDECLTYPE, kept for the trace
  uint32_t guest_offset = 0;
  uint32_t guest_stream = 0;
  bool integer = false;
};

struct HostLayout {
  std::vector<HostElement> elements;
  uint64_t key = 0;
  uint32_t stride() const { return uint32_t(elements.size()) * 16; }
};

enum class DrawTopology : uint32_t { PointList, LineList, LineStrip, TriangleList, TriangleStrip };

enum class CommandKind : uint32_t { Draw, Clear, Resolve };

// A draw's converted vertices and indices, kept across frames while the guest bytes they came from stay the same; the present uploads them once.
struct HostGeometry {
  std::vector<uint8_t> vertices;
  std::vector<uint32_t> indices;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  DrawTopology topology = DrawTopology::TriangleList;
  std::shared_ptr<plume::RenderBuffer> vertex_buffer;
  std::shared_ptr<plume::RenderBuffer> index_buffer;
};

// A guest surface: its Color0Info or DepthInfo word (EDRAM base and format) and size, which is what a host render target is keyed by.
struct SurfaceBinding {
  uint32_t info = 0;
  uint32_t surface_info = 0;  // GPU_SURFACEINFO: pitch in pixels bits 0..13, MsaaSamples bits 16..17
  uint32_t width = 0;
  uint32_t height = 0;
  bool valid() const { return width != 0 && height != 0; }
};

// D3DDevice_SetTexture puts sampler N in fetch constant N: pixel samplers are 0..15 and vertex texture fetch reads 16..19.
inline constexpr uint32_t kPixelSamplers = 16;
inline constexpr uint32_t kSamplerSlots = 20;

struct DrawRecord {
  CommandKind kind = CommandKind::Draw;
  SurfaceBinding color;
  SurfaceBinding depth;
  // Render targets 1..3, which the pixel shader's oC1..oC3 write.
  SurfaceBinding extra_colors[3];
  const ShaderCacheEntry* vs = nullptr;
  const ShaderCacheEntry* ps = nullptr;
  const HostLayout* layout = nullptr;
  DrawTopology topology = DrawTopology::TriangleList;
  // The cached geometry, or null when the vertices and indices are in the arena.
  std::shared_ptr<HostGeometry> geometry;
  // The guest index of host vertex 0: a vertex shader starts with the vertex index in r0.x.
  int32_t vertex_index_base = 0;
  // Where the present put the vertices in the vertex atlas, for a shader that fetches at computed indices; ~0 when it did not.
  uint32_t vertex_atlas_base = ~0u;
  // Offsets into DrawFrame::arena.
  uint32_t vertices = 0;
  uint32_t vertex_count = 0;
  uint32_t indices = 0;
  uint32_t index_count = 0;
  uint32_t constants = 0;  // VertexConstantRegisters float4s, then PixelConstantRegisters, little-endian
  uint32_t booleans = 0;
  uint32_t blend_control = 0;
  uint32_t extra_blend_controls[3] = {};
  uint32_t depth_control = 0;
  uint32_t color_mask = 0xF;  // GPU_COLORMASK: four bits per render target
  uint32_t color_control = 0;
  uint32_t mode_control = 0;
  // GPU_POINTPACKET.PolyOffsetFront{Scale,Offset} when ModeControl enables polygon offset (bits 11 and 12), else 0.
  float poly_offset_scale = 0.0f;
  float poly_offset = 0.0f;
  float alpha_ref = 0.0f;
  uint32_t stencil_ref_mask = 0;
  int32_t scissor[4] = {};  // left, top, right, bottom in target pixels
  float viewport[6] = {};  // Vport X/Y/Z scale and offset
  uint32_t vte_control = 0;  // GPU_VTECONTROL: which of them apply
  uint32_t vtx_control = 0;  // GPU_VTXCONTROL: PixCenter 0 puts pixel centres on integer coordinates
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  // Samplers with a texture bound (pixel 0..15, vertex 16..19); their fetch constants, and the bindless indices the present resolves them to.
  uint32_t texture_mask = 0;
  TextureFetch textures[kSamplerSlots];
  uint32_t texture_index[kSamplerSlots] = {};
  uint32_t texture_3d_index[16] = {};
  uint32_t texture_cube_index[16] = {};
  uint32_t sampler_index[kSamplerSlots] = {};
  // Pixel samplers whose 2D texture is a resolve, so at the render resolution scale; the shaders scale texel offsets for them.
  uint32_t scaled_textures = 0;

  // Clear: D3DDevice_ClearF's flags (bits 0..3 targets, 0x10 depth, 0x20 stencil), rectangle and values.
  uint32_t clear_flags = 0;
  bool clear_has_rect = false;
  int32_t clear_rect[4] = {};
  float clear_color[4] = {};
  float clear_depth = 1.0f;

  // Resolve: the source surface (colour target 0..3 by flags & 7, 4 for depth), the source rectangle, the destination texture and where in it.
  uint32_t resolve_flags = 0;
  SurfaceBinding resolve_source;
  int32_t resolve_rect[4] = {};
  TextureFetch resolve_dest;
  int32_t resolve_point[2] = {};
  uint32_t resolve_slice = 0;  // DestSliceOrFace: the z of a volume destination
  float resolve_clear_color[4] = {};
  float resolve_clear_depth = 1.0f;
};

struct DrawFrame {
  std::vector<uint8_t> arena;
  std::vector<DrawRecord> draws;
  uint32_t dropped_no_shader = 0;
  uint32_t dropped_no_layout = 0;
  uint32_t dropped_other = 0;

  uint32_t Alloc(size_t bytes);
  void Clear();
};

// The float4 constant registers a draw's arena block holds per stage: as many as its shader reads.
inline uint32_t VertexConstantRegisters(const DrawRecord& record) { return record.vs ? record.vs->constantRegisterCount : 0; }
inline uint32_t PixelConstantRegisters(const DrawRecord& record) { return record.ps ? record.ps->constantRegisterCount : 0; }

// Called from the draw hooks: `start` and `count` are vertices for a non-indexed draw, indices into the bound index buffer otherwise.
void CaptureDraw(uint32_t device, uint32_t primitive_type, uint32_t start, uint32_t count, bool indexed);
// For draws whose vertices come from guest memory the device does not describe: DrawVerticesUP (read now) and the Begin*Vertices pointers (`deferred`, read once the guest has filled them).
// `index_format` is the D3DFORMAT of the indices; `base_vertex` is added to each, as BeginIndexedVertices sets VGT_INDX_OFFSET.
void CaptureDrawFromMemory(uint32_t device, uint32_t primitive_type, uint32_t vertex_data, uint32_t vertex_count, uint32_t stride, uint32_t index_data, uint32_t index_count, uint32_t index_format, int32_t base_vertex, bool deferred);

// D3DDevice_ClearF and D3DDevice_Resolve, recorded in order with the draws.
void CaptureClear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, float depth);
void CaptureResolve(uint32_t device, uint32_t flags, uint32_t source_rect, uint32_t dest_texture, uint32_t dest_point, uint32_t dest_slice, uint32_t clear_color, float clear_depth);

// What a host render target is keyed by besides its size: the EDRAM base and, for colour, the format.
uint32_t SurfaceKey(uint32_t info, bool depth);

// Hands the captured frame to the caller and starts a new one.
void TakeCapturedFrame(DrawFrame& out);

}  // namespace cherry::gpu
