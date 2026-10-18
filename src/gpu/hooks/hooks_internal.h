// Shared by the REX_HOOK translation units that replace the guest D3D library.
#pragma once

#include <cstdint>

namespace cherry::gpu::hooks {

// UE3's GDirect3DDevice; Direct3D_CreateDevice stores the device there.
inline constexpr uint32_t kGDirect3DDevice = 0x8332FDDC;

uint32_t GuestDevice();

// Nothing consumes the guest ring, so it hands out plain guest memory: a command sink nothing reads and a per-frame data sink for vertex and index data.
bool RingSinksReady();
uint32_t DataSinkAlloc(uint32_t bytes, uint32_t alignment);
void ResetDataSink();
// Lays a fresh command segment over the sink as D3D_CDevice_StartNewSegment would, and returns the new write pointer.
uint32_t RewindCommandRing(uint32_t device);

// Logs how the stubbed ring has been used and whether the device's command write pointer is inside the sink.
void LogRingStats(uint32_t device);

// The native viewport and scissor setters (state.cpp), shared with the render-target setters that reset them.
void SetGuestScissorRect(uint32_t device, uint32_t rect);
void SetGuestViewport(uint32_t device, float x, float y, float w, float h, float min_z, float max_z);

// Writes the CPU fence into the GPU read-back word, which resolves every fence wait, inlined ones included; with no guest GPU, everything queued is done.
void PublishFences(uint32_t device);

}  // namespace cherry::gpu::hooks
