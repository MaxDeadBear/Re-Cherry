#pragma once

namespace plume {
struct RenderDevice;
}  // namespace plume

namespace cherry::gpu::debug {

// With --cherry_debug_dred, turns on D3D12 auto-breadcrumbs and page-fault reporting; must run before the device is created.
void EnableDred();

// Logs, once, the D3D12 breadcrumbs and page fault of a removed device: each command list's operations around the last one the GPU finished.
void LogDred(plume::RenderDevice* device);

}  // namespace cherry::gpu::debug
