#include "gpu/graphics_system.h"

#include <plume_d3d12.h>
#include <plume_render_interface.h>
#include <plume_vulkan.h>

#include "gpu/debug/screenshot.h"

namespace cherry::gpu {

namespace {

constexpr plume::RenderFormat kCaptureFormat = plume::RenderFormat::B8G8R8A8_UNORM;

uint32_t CapturePitch(uint32_t width) {
  return (width * 4 + 255) & ~255u;
}

}  // namespace

void GraphicsSystem::CaptureNextFrame(FrameCapture done) {
  std::lock_guard<std::mutex> lock(render_mutex_);
  next_capture_ = std::move(done);
}

bool GraphicsSystem::RecordFrameCapture(plume::RenderCommandList* list, const TextureFetch& front_buffer) {
  const uint32_t width = front_buffer.width(), height = front_buffer.height();
  if (!device_ || !width || !height) return false;
  if (!capture_target_ || capture_width_ != width || capture_height_ != height) {
    capture_framebuffer_.reset();
    capture_target_ = device_->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kCaptureFormat));
    capture_readback_ = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(CapturePitch(width)) * height));
    if (capture_target_ && capture_readback_) {
      const plume::RenderTexture* attachment = capture_target_.get();
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &attachment;
      desc.colorAttachmentsCount = 1;
      capture_framebuffer_ = device_->createFramebuffer(desc);
    }
    capture_width_ = capture_framebuffer_ ? width : 0;
    capture_height_ = capture_framebuffer_ ? height : 0;
    if (!capture_framebuffer_) return false;
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(capture_target_.get(), plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(capture_framebuffer_.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  // At the front buffer's own size the blit's scale is exactly 1, so every texel lands on its pixel.
  const bool drawn = BlitFrontBuffer(list, front_buffer, width, height);
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(capture_target_.get(), plume::RenderTextureLayout::COPY_SOURCE));
  if (drawn) debug::CopyToReadback(list, capture_target_.get(), capture_readback_.get(), width, height, CapturePitch(width) / 4);
  return drawn;
}

void* GraphicsSystem::RenderDocDevice() const {
  if (auto* d3d12 = dynamic_cast<plume::D3D12Device*>(device_.get())) return d3d12->d3d;
  // RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE: the instance's dispatch table pointer.
  if (auto* vulkan = dynamic_cast<plume::VulkanInterface*>(interface_.get()); vulkan && vulkan->instance) return *reinterpret_cast<void**>(vulkan->instance);
  return nullptr;
}

void GraphicsSystem::DeliverFrameCapture(bool recorded) {
  FrameCapture done = std::move(render_capture_);
  render_capture_ = nullptr;
  if (!done) return;
  const uint8_t* pixels = recorded && capture_readback_ ? static_cast<const uint8_t*>(capture_readback_->map()) : nullptr;
  done(pixels, pixels ? capture_width_ : 0, pixels ? capture_height_ : 0, CapturePitch(capture_width_));
  if (pixels) capture_readback_->unmap();
}

}  // namespace cherry::gpu
