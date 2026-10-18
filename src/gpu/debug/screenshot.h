#pragma once

#include <cstdint>
#include <filesystem>

namespace plume {
struct RenderCommandList;
struct RenderTexture;
struct RenderBuffer;
}  // namespace plume

namespace cherry::gpu::debug {

// Presented frames between captures under --cherry_debug_screenshot_every, 0 when off.
uint32_t ScreenshotInterval();

// bind_debug_frames (F8): the next kBurstFrames presented frames are written like --cherry_debug_screenshot_every and, under RenderDoc, captured one by one.
inline constexpr uint32_t kBurstFrames = 3;
void RegisterFrameBurstBind();
// On the render thread, once per frame before it is replayed: true while a requested burst lasts.
bool TakeBurstFrame();

// Records a copy of a BGRA8 texture, already in COPY_SOURCE, into a readback buffer with rows `row_texels` apart.
void CopyToReadback(plume::RenderCommandList* list, plume::RenderTexture* source, plume::RenderBuffer* buffer, uint32_t width, uint32_t height, uint32_t row_texels);

// Writes a top-down BGRA8 image with rows `pitch` bytes apart as an uncompressed TGA.
bool WriteTga(const std::filesystem::path& path, const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch);

}  // namespace cherry::gpu::debug
