#pragma once

namespace cherry::gpu {
struct DrawFrame;
}  // namespace cherry::gpu

namespace cherry::gpu::debug {

// With --cherry_debug_edram_alias, follows which surface last wrote each EDRAM tile and logs, every 150 frames, the draws and resolves that read tiles another surface wrote.
void TrackEdram(const DrawFrame& frame);

}  // namespace cherry::gpu::debug
