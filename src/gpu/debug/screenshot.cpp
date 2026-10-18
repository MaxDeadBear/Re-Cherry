#include "gpu/debug/screenshot.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <vector>

#include <plume_render_interface.h>
#include <plume_vulkan.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

REXCVAR_DEFINE_INT32(cherry_debug_screenshot_every, 0, "Lollipop/Debug", "Write every Nth presented frame to shots/frame_NNNNNN.tga, read back from the swap chain; 0 is off.");

namespace cherry::gpu::debug {

namespace {

std::atomic<uint32_t> g_burst_requested{0};
uint32_t g_burst_left = 0;

}  // namespace

uint32_t ScreenshotInterval() {
  return uint32_t(std::max(REXCVAR_GET(cherry_debug_screenshot_every), 0));
}

void RegisterFrameBurstBind() {
  rex::ui::RegisterBind("bind_debug_frames", "F8", "Debug: write the next frames to shots/ and capture them under RenderDoc", [] { g_burst_requested = kBurstFrames; });
}

bool TakeBurstFrame() {
  if (const uint32_t requested = g_burst_requested.exchange(0)) {
    g_burst_left = requested;
    REXGPU_INFO("[gfx] capturing the next {} frames", requested);
  }
  if (!g_burst_left) return false;
  --g_burst_left;
  return true;
}

void CopyToReadback(plume::RenderCommandList* list, plume::RenderTexture* source, plume::RenderBuffer* buffer, uint32_t width, uint32_t height, uint32_t row_texels) {
  // plume's Vulkan copyTextureRegion has no image-to-buffer path, so that copy is recorded directly.
  if (auto* vk_list = dynamic_cast<plume::VulkanCommandList*>(list)) {
    vk_list->endActiveRenderPass();
    VkBufferImageCopy region = {};
    region.bufferRowLength = row_texels;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(vk_list->vk, static_cast<plume::VulkanTexture*>(source)->vk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<plume::VulkanBuffer*>(buffer)->vk, 1, &region);
    return;
  }
  auto dst = plume::RenderTextureCopyLocation::PlacedFootprint(buffer, plume::RenderFormat::B8G8R8A8_UNORM, width, height, 1, row_texels, 0);
  // plume's D3D12 copyTextureRegion reads dst.texture for its sample positions even for a placed footprint.
  dst.texture = source;
  list->copyTextureRegion(dst, plume::RenderTextureCopyLocation::Subresource(source, 0, 0));
}

bool WriteTga(const std::filesystem::path& path, const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  // Uncompressed true-colour, 24 bits per pixel, origin top-left (descriptor bit 5); the swap chain's alpha is whatever the game left there, so it is dropped.
  const uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(width), uint8_t(width >> 8), uint8_t(height), uint8_t(height >> 8), 24, 0x20};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  std::vector<uint8_t> row(size_t(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = bgra + size_t(y) * pitch;
    for (uint32_t x = 0; x < width; ++x) std::memcpy(&row[size_t(x) * 3], src + size_t(x) * 4, 3);
    out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
  }
  return bool(out);
}

}  // namespace cherry::gpu::debug
