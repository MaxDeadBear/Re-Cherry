// Compare mode: the game's own D3D runs and feeds Xenia's GPU plugin, and these hooks hand the same draws, clears, resolves and swaps to the
// native renderer. They are raw so the original runs with the guest's untouched register context (stack arguments, f1), unlike the
// main build's typed REX_HOOKs in src/gpu/hooks/, which this build leaves out.

#include <algorithm>
#include <cstring>

#include "re_cherry_init.h"

#include "compare/xenia/paired_capture.h"

#include <rex/hook.h>

#include "engine/frame_rate.h"
#include "gpu/draw/draw_frame.h"
#include "gpu/graphics_system.h"
#include "gpu/guest_memory.h"
#include "gpu/shaders/shader_registry.h"

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_BeginIndexedVertices);
REX_EXTERN(__imp__D3DDevice_ClearF);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_Swap);
REX_EXTERN(__imp__D3DDevice_BlockUntilIdle);
REX_EXTERN(__imp__D3DVertexShader_Bind);
REX_EXTERN(__imp__XGRegisterVertexShader);
REX_EXTERN(__imp__XGRegisterPixelShader);

namespace {

using namespace cherry::gpu;

// The ninth integer argument, on the caller's stack.
uint32_t StackArgument9(PPCContext& ctx) {
  return LoadGuestU32(ctx.r1.u32 + 0x54);
}

}  // namespace

// D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex, UINT VertexCount)
REX_HOOK_RAW(D3DDevice_DrawVertices) {
  CaptureDraw(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, false);
  __imp__D3DDevice_DrawVertices(ctx, base);
}

// D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartIndex, UINT IndexCount)
REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  CaptureDraw(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, true);
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
}

// D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount, UINT Stride) -> void*; DrawVerticesUP comes through here too.
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  const uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__D3DDevice_BeginVertices(ctx, base);
  if (const uint32_t vertices = ctx.r3.u32) CaptureDrawFromMemory(device, primitive, vertices, count, stride, 0, 0, 0, 0, true);
}

// D3DDevice_BeginIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT NumVertices, UINT IndexCount, D3DFORMAT, UINT Stride, void** ppIndexData, void** ppVertexData)
REX_HOOK_RAW(D3DDevice_BeginIndexedVertices) {
  const uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, base_vertex = ctx.r5.u32, vertices = ctx.r6.u32, index_count = ctx.r7.u32, format = ctx.r8.u32, stride = ctx.r9.u32;
  const uint32_t index_out = ctx.r10.u32, vertex_out = StackArgument9(ctx);
  __imp__D3DDevice_BeginIndexedVertices(ctx, base);
  if (ctx.r3.u32 != 0 || !index_out || !vertex_out) return;
  CaptureDrawFromMemory(device, primitive, LoadGuestU32(vertex_out), vertices, stride, LoadGuestU32(index_out), index_count, format, int32_t(base_vertex), true);
}

// D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT*, const D3DVECTOR4* pColor, float Z)
REX_HOOK_RAW(D3DDevice_ClearF) {
  CaptureClear(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64));
  __imp__D3DDevice_ClearF(ctx, base);
}

// D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT*, D3DBaseTexture*, const D3DPOINT*, UINT DestLevel/Slice, const D3DVECTOR4* pClearColor, float ClearZ, ...)
REX_HOOK_RAW(D3DDevice_Resolve) {
  CaptureResolve(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, float(ctx.f1.f64));
  __imp__D3DDevice_Resolve(ctx, base);
}

// D3DDevice_Swap(D3DDevice*, D3DBaseTexture* pFrontBuffer, const D3DVIDEO_SCALER_PARAMETERS*)
REX_HOOK_RAW(D3DDevice_Swap) {
  const uint32_t device = ctx.r3.u32, front_buffer = ctx.r4.u32;
  TextureFetch front;
  for (uint32_t w = 0; front_buffer && w < 6; ++w) front.words[w] = LoadGuestU32(front_buffer + 0x1C + 4 * w);
  __imp__D3DDevice_Swap(ctx, base);
  static DrawFrame frame;
  TakeCapturedFrame(frame);
  const bool capture = cherry::compare::BeginPairedCapture();
  if (GraphicsSystem* graphics = GraphicsSystem::instance()) graphics->SubmitFrame(frame, front);
  // The game's own D3DDevice_BlockUntilIdle waits on the fence after the swap packet, so Xenia has drawn this frame when it returns.
  if (capture) cherry::compare::FinishPairedCapture([&] {
    ctx.r3.u64 = device;
    __imp__D3DDevice_BlockUntilIdle(ctx, base);
  });
  cherry::engine::NotePresentedFrame();
}

// D3DVertexShader_Bind(D3DVertexShader*, D3DVertexDeclaration*, const DWORD* strides, D3DPixelShader*)
REX_HOOK_RAW(D3DVertexShader_Bind) {
  const uint32_t vertex_shader = ctx.r3.u32, declaration = ctx.r4.u32, strides = ctx.r5.u32;
  __imp__D3DVertexShader_Bind(ctx, base);
  if (!vertex_shader || !declaration) return;
  // The declaration's highest stream at +28, as Bind reads it.
  VertexShaderBinding binding;
  binding.declaration = declaration;
  const uint32_t max_stream = std::min(LoadGuestU32(declaration + 28), 15u);
  for (uint32_t s = 0; strides && s <= max_stream; ++s) binding.strides[s] = uint8_t(LoadGuestU32(strides + 4 * s) >> 2);
  NoteVertexShaderBinding(vertex_shader, binding);
}

// XGRegisterVertexShader(D3DVertexShader*, const void* pPhysicalPart); the container follows the object at +0x368.
REX_HOOK_RAW(XGRegisterVertexShader) {
  const uint32_t shader = ctx.r3.u32, physical = ctx.r4.u32;
  __imp__XGRegisterVertexShader(ctx, base);
  RegisterShader(GuestBase(), shader + 0x368, physical, shader, false);
}

// XGRegisterPixelShader(D3DPixelShader*, const void* pPhysicalPart); XePixelShader_Create copied the container's virtual part to +0x28.
REX_HOOK_RAW(XGRegisterPixelShader) {
  const uint32_t shader = ctx.r3.u32, physical = ctx.r4.u32;
  __imp__XGRegisterPixelShader(ctx, base);
  RegisterShader(GuestBase(), shader + 0x28, physical, shader, true);
}
