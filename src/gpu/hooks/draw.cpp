#include "gpu/hooks/hooks_internal.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/draw/draw_frame.h"
#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"

// Draws, clears and resolves are captured from the device's shadow state and replayed at the present; predicated tiling is dropped, and logged if the game ever uses it. The guest no longer builds packets for a ring nothing reads.

namespace {

using namespace cherry::gpu;
using namespace cherry::gpu::hooks;

constexpr uint32_t kIndexFormat32Bit = 0x4;

// EndVertices is inlined as `dev+0x30 = dev+0x3614`, so pointing the resume pointer at the current write pointer makes it a no-op.
void ParkVerticesResume(uint32_t device) {
  StoreGuestU32(device + dev::kVerticesResumePtr, LoadGuestU32(device + dev::kCmdWritePtr));
}

void D3DDevice_DrawVertices_hook(uint32_t device, uint32_t primitive_type, uint32_t start_vertex, uint32_t vertex_count) {
  CaptureDraw(device, primitive_type, start_vertex, vertex_count, false);
}

void D3DDevice_DrawIndexedVertices_hook(uint32_t device, uint32_t primitive_type, uint32_t start_index, uint32_t index_count) {
  CaptureDraw(device, primitive_type, start_index, index_count, true);
}

uint32_t D3DDevice_DrawVerticesUP_hook(uint32_t device, uint32_t primitive_type, uint32_t vertex_count, uint32_t vertex_data, uint32_t stride) {
  CaptureDrawFromMemory(device, primitive_type, vertex_data, vertex_count, stride, 0, 0, 0, 0, false);
  return 0;
}

uint32_t D3DDevice_BeginVertices_hook(uint32_t device, uint32_t primitive_type, uint32_t vertex_count, uint32_t stride) {
  if (!RingSinksReady()) return 0;
  ParkVerticesResume(device);
  const uint32_t vertices = DataSinkAlloc(vertex_count * stride, 16);
  CaptureDrawFromMemory(device, primitive_type, vertices, vertex_count, stride, 0, 0, 0, 0, true);
  return vertices;
}

uint32_t D3DDevice_BeginIndexedVertices_hook(uint32_t device, uint32_t primitive_type, uint32_t base_vertex_index, uint32_t num_vertices, uint32_t index_count, uint32_t index_format, uint32_t stride, uint32_t index_data_out, uint32_t vertex_data_out) {
  constexpr uint32_t kOutOfMemory = 0x8007000E;
  if (!RingSinksReady()) return kOutOfMemory;
  ParkVerticesResume(device);
  const bool index_32bit = (index_format & kIndexFormat32Bit) != 0;
  const uint32_t indices = DataSinkAlloc(index_count * (index_32bit ? 4 : 2), 16);
  const uint32_t vertices = DataSinkAlloc(num_vertices * stride, 16);
  StoreGuestU32(index_data_out, indices);
  StoreGuestU32(vertex_data_out, vertices);
  CaptureDrawFromMemory(device, primitive_type, vertices, num_vertices, stride, indices, index_count, index_format, int32_t(base_vertex_index), true);
  return 0;
}

void D3DDevice_ClearF_hook(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, double z) {
  CaptureClear(device, flags, rect, color, float(z));
}

// ClearZ is in f1 and still uses up r10, so the placeholder keeps ClearStencil on the 9th integer slot, the caller's stack.
void D3DDevice_Resolve_hook(uint32_t device, uint32_t flags, uint32_t source_rect, uint32_t dest_texture, uint32_t dest_point, uint32_t dest_slice, uint32_t clear_color, double clear_z, uint32_t clear_z_gpr_slot, uint32_t clear_stencil) {
  (void)clear_z_gpr_slot;
  (void)clear_stencil;
  CaptureResolve(device, flags, source_rect, dest_texture, dest_point, dest_slice, clear_color, float(clear_z));
}

void D3DDevice_BeginTiling_hook(uint32_t device, uint32_t flags, uint32_t count, uint32_t tile_rects, double clear_z) {
  (void)device;
  (void)tile_rects;
  (void)clear_z;
  static uint32_t logged = 0;
  if (logged++ < 4) REXGPU_WARN("[gfx] BeginTiling (flags {:08X}, {} tiles) is not replayed; its EndTiling resolves are lost", flags, count);
}

void D3DDevice_EndTiling_hook(uint32_t device) {
  (void)device;
}

}  // namespace

// D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex, UINT VertexCount)
REX_HOOK(D3DDevice_DrawVertices, D3DDevice_DrawVertices_hook)
// D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartIndex, UINT IndexCount)
REX_HOOK(D3DDevice_DrawIndexedVertices, D3DDevice_DrawIndexedVertices_hook)
// D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount, const void* pVertexData, UINT Stride)
REX_HOOK(D3DDevice_DrawVerticesUP, D3DDevice_DrawVerticesUP_hook)
// D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount, UINT Stride) -> void*
REX_HOOK(D3DDevice_BeginVertices, D3DDevice_BeginVertices_hook)
// D3DDevice_BeginIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT NumVertices, UINT IndexCount, D3DFORMAT, UINT Stride, void** ppIndexData, void** ppVertexData) -> HRESULT
REX_HOOK(D3DDevice_BeginIndexedVertices, D3DDevice_BeginIndexedVertices_hook)
// D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT*, const D3DVECTOR4* pColor, float Z)
REX_HOOK(D3DDevice_ClearF, D3DDevice_ClearF_hook)
// D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT* pSourceRect, D3DBaseTexture* pDestTexture, const D3DPOINT* pDestPoint, UINT DestSliceOrFace, const D3DVECTOR4* pClearColor, float ClearZ, DWORD ClearStencil)
REX_HOOK(D3DDevice_Resolve, D3DDevice_Resolve_hook)
// D3DDevice_BeginTiling(D3DDevice*, DWORD Flags, DWORD Count, const D3DRECT* pTileRects, float ClearZ)
REX_HOOK(D3DDevice_BeginTiling, D3DDevice_BeginTiling_hook)
// D3DDevice_EndTiling(D3DDevice*)
REX_HOOK(D3DDevice_EndTiling, D3DDevice_EndTiling_hook)
