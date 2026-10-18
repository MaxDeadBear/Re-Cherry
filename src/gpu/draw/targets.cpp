#include <algorithm>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/draw/draw_frame.h"
#include "gpu/graphics_system.h"

// Host render targets standing in for EDRAM: one per guest surface, keyed by its RB_*_INFO word (EDRAM base and format) and size, at the guest's own resolution.

namespace cherry::gpu {

namespace {

constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;

// Color0Info's format, bits 16..19; the host has no 10:10:10:2 format, so those take 16-bit floats.
plume::RenderFormat ColorTargetFormat(uint32_t info) {
  switch ((info >> 16) & 0xF) {
    case 0:
    case 1: return plume::RenderFormat::R8G8B8A8_UNORM;
    case 4:
    case 6: return plume::RenderFormat::R16G16_FLOAT;
    case 14: return plume::RenderFormat::R32_FLOAT;
    case 15: return plume::RenderFormat::R32G32_FLOAT;
    default: return plume::RenderFormat::R16G16B16A16_FLOAT;
  }
}

}  // namespace

// EDRAM base (bits 0..11) and format (bits 16..19); the other bits are exponent bias and do not change what the surface holds. Formats sharing tiles stay apart: UE3 saves HDR scene bits through the 2_10_10_10 view and restores them after borrowing the tiles, which host float targets need not do.
uint32_t SurfaceKey(uint32_t info, bool depth) {
  return info & (depth ? 0x00000FFFu : 0x000F0FFFu);
}

GraphicsSystem::HostTarget* GraphicsSystem::Target(const SurfaceBinding& surface, bool depth) {
  if (!surface.valid()) return nullptr;
  HostTarget& target = targets_[std::tuple{SurfaceKey(surface.info, depth), surface.width, surface.height, depth}];
  if (target.texture) return &target;
  const plume::RenderFormat format = depth ? kDepthFormat : ColorTargetFormat(surface.info);
  const float scale = TargetScale(surface.width, surface.height);
  const uint32_t host_width = TextureCache::Scaled(surface.width, scale);
  const uint32_t host_height = TextureCache::Scaled(surface.height, scale);
  target.texture = device_->createTexture(depth ? plume::RenderTextureDesc::DepthTarget(host_width, host_height, format) : plume::RenderTextureDesc::ColorTarget(host_width, host_height, format));
  if (!target.texture) {
    REXGPU_ERROR("[gfx] render target {}x{} ({}) creation failed", host_width, host_height, depth ? "depth" : "colour");
    return nullptr;
  }
  target.view = target.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(format));
  target.format = uint32_t(format);
  target.width = surface.width;
  target.height = surface.height;
  target.host_width = host_width;
  target.host_height = host_height;
  target.scale = scale;
  target.depth = depth;
  target.layout = uint32_t(plume::RenderTextureLayout::UNKNOWN);
  if ((target.heap_index = textures_.ReserveIndex())) draw_sets_[0]->setTexture(target.heap_index, target.texture.get(), depth ? plume::RenderTextureLayout::DEPTH_READ : plume::RenderTextureLayout::SHADER_READ, target.view.get());
  static uint32_t logged = 0;
  if (logged++ < 16) REXGPU_INFO("[gfx] render target {}x{} {} (EDRAM info {:08X})", surface.width, surface.height, depth ? "depth" : "colour", surface.info);
  return &target;
}

GraphicsSystem::ColorTargets GraphicsSystem::DrawTargets(const DrawRecord& record) {
  ColorTargets colors = {Target(record.color, false)};
  for (uint32_t i = 1; i < 4 && colors[i - 1]; ++i) {
    colors[i] = Target(record.extra_colors[i - 1], false);
    if (std::find(colors.begin(), colors.begin() + i, colors[i]) != colors.begin() + i) colors[i] = nullptr;
  }
  return colors;
}

GraphicsSystem::HostTarget* GraphicsSystem::MsaaAlias(const SurfaceBinding& surface, uint32_t* samples_x, uint32_t* samples_y) {
  // GPU_SURFACEINFO.MsaaSamples: 1 is 2x, two samples a pixel stacked vertically; 2 is 4x, a 2x2 block.
  const uint32_t msaa = (surface.surface_info >> 16) & 3;
  if (!surface.valid() || msaa == 0) return nullptr;
  *samples_x = msaa == 2 ? 2 : 1;
  *samples_y = 2;
  auto found = targets_.find(std::tuple{SurfaceKey(surface.info, false), surface.width * *samples_x, surface.height * *samples_y, false});
  return found != targets_.end() && found->second.texture ? &found->second : nullptr;
}

plume::RenderFramebuffer* GraphicsSystem::TargetFramebuffer(const ColorTargets& colors, HostTarget* depth) {
  auto& framebuffer = target_framebuffers_[std::pair{colors, static_cast<const HostTarget*>(depth)}];
  if (framebuffer) return framebuffer.get();
  const plume::RenderTexture* attachments[4] = {};
  uint32_t count = 0;
  while (count < 4 && colors[count]) {
    attachments[count] = colors[count]->texture.get();
    ++count;
  }
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = count ? attachments : nullptr;
  desc.colorAttachmentsCount = count;
  if (depth) {
    desc.depthAttachment = depth->texture.get();
    desc.depthAttachmentView = depth->view.get();
  }
  framebuffer = device_->createFramebuffer(desc);
  return framebuffer.get();
}

void GraphicsSystem::TransitionTarget(plume::RenderCommandList* list, HostTarget* target, uint32_t layout) {
  if (!target || target->layout == layout) return;
  list->barriers(layout == uint32_t(plume::RenderTextureLayout::COPY_SOURCE) ? plume::RenderBarrierStage::COPY : plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(target->texture.get(), plume::RenderTextureLayout(layout)));
  target->layout = layout;
}

}  // namespace cherry::gpu
