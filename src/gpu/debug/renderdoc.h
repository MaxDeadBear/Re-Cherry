#pragma once

#include <cstdint>

namespace cherry::gpu::debug {

// Called once per present; when the game runs under RenderDoc, captures the presented frames --cherry_debug_renderdoc_frames lists.
void RenderDocBeforePresent(uint64_t presented_frame);
// The vertex shader --cherry_debug_renderdoc_on_shader names (hex hash), or 0; the replay calls RenderDocShaderSeen when a frame draws with it.
uint64_t RenderDocTriggerShader();
// Arms a capture of the next frame, at most twice and 8 frames apart.
void RenderDocShaderSeen(uint64_t presented_frame);
// Under RenderDoc, captures only what `device` (RenderDoc's device pointer) does until RenderDocEndDeviceCapture, whatever else in the process presents; false when RenderDoc is not attached.
bool RenderDocBeginDeviceCapture(void* device, void* window);
void RenderDocEndDeviceCapture(void* device, void* window);

}  // namespace cherry::gpu::debug
