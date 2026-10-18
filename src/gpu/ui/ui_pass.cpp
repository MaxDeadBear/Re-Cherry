#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/graphics_system.h"
#include "gpu/shaders/builtin_shaders.h"

namespace cherry::gpu {

namespace {

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr plume::RenderFormat kUiTextureFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr uint32_t kCopyPlacementAlign = 512;
constexpr uint32_t kCopyRowAlign = 256;

plume::RenderDescriptorSetDesc UiDescriptorSetDesc() {
  static const plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  return plume::RenderDescriptorSetDesc(ranges, uint32_t(std::size(ranges)));
}

}  // namespace

uint32_t GraphicsSystem::CreateUiTexture(uint32_t width, uint32_t height, const uint8_t* rgba) {
  if (!width || !height || !rgba) return 0;
  UiTexture entry;
  entry.width = width;
  entry.height = height;
  entry.pixels.assign(rgba, rgba + size_t(width) * height * 4);
  std::lock_guard<std::mutex> lock(ui_mutex_);
  const uint32_t id = ui_next_texture_++;
  ui_textures_.emplace(id, std::move(entry));
  return id;
}

void GraphicsSystem::SubmitUi(UiFrame&& frame) {
  std::lock_guard<std::mutex> lock(ui_mutex_);
  ui_pending_ = std::move(frame);
  ui_pending_fresh_ = true;
}

void GraphicsSystem::SetUiFrameRequest(std::function<void()> request) {
  std::lock_guard<std::mutex> lock(ui_mutex_);
  ui_frame_request_ = std::move(request);
}

void GraphicsSystem::RequestUiFrame() {
  std::function<void()> request;
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    request = ui_frame_request_;
  }
  if (request) request();
}

bool GraphicsSystem::EnsureUiPipeline() {
  if (ui_pipeline_) return true;
  if (ui_pipeline_failed_ || !device_) return false;
  ui_pipeline_failed_ = true;

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  ui_sampler_ = device_->createSampler(sampler_desc);

  const plume::RenderDescriptorSetDesc set_desc = UiDescriptorSetDesc();
  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  layout_desc.allowInputLayout = true;
  ui_layout_ = device_->createPipelineLayout(layout_desc);

  if (using_vulkan_) {
    ui_vs_ = device_->createShader(shaders::g_uiVSSpirv, sizeof(shaders::g_uiVSSpirv), "VSMain", plume::RenderShaderFormat::SPIRV);
    ui_ps_ = device_->createShader(shaders::g_uiPSSpirv, sizeof(shaders::g_uiPSSpirv), "PSMain", plume::RenderShaderFormat::SPIRV);
  } else {
    ui_vs_ = device_->createShader(shaders::g_uiVSDxil, sizeof(shaders::g_uiVSDxil), "VSMain", plume::RenderShaderFormat::DXIL);
    ui_ps_ = device_->createShader(shaders::g_uiPSDxil, sizeof(shaders::g_uiPSDxil), "PSMain", plume::RenderShaderFormat::DXIL);
  }
  if (!ui_sampler_ || !ui_layout_ || !ui_vs_ || !ui_ps_) {
    REXGPU_ERROR("[gfx] overlay shaders or layout failed; the ImGui overlays are not drawn");
    return false;
  }

  const plume::RenderInputSlot slot(0, sizeof(UiVertex));
  const plume::RenderInputElement elements[] = {
      plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0, offsetof(UiVertex, x)),
      plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, offsetof(UiVertex, u)),
      plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R32G32B32A32_FLOAT, 0, offsetof(UiVertex, r)),
  };
  plume::RenderGraphicsPipelineDesc desc;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements;
  desc.inputElementsCount = uint32_t(std::size(elements));
  desc.pipelineLayout = ui_layout_.get();
  desc.vertexShader = ui_vs_.get();
  desc.pixelShader = ui_ps_.get();
  desc.renderTargetFormat[0] = kColorFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
  desc.renderTargetCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  ui_pipeline_ = device_->createGraphicsPipeline(desc);
  if (!ui_pipeline_) {
    REXGPU_ERROR("[gfx] overlay pipeline creation failed; the ImGui overlays are not drawn");
    return false;
  }
  ui_pipeline_failed_ = false;
  REXGPU_INFO("[gfx] overlay pipeline ready");
  return true;
}

// Textures registered on the UI thread since the last present are created and copied here, each into its own region of this slot's upload buffer.
void GraphicsSystem::UploadUiTextures(plume::RenderCommandList* list) {
  struct PendingUpload {
    UiTexture* texture = nullptr;
    uint32_t offset = 0;
    uint32_t pitch = 0;
  };
  std::vector<PendingUpload> pending;
  uint32_t staged = 0;
  std::lock_guard<std::mutex> lock(ui_mutex_);
  for (auto& [id, tex] : ui_textures_) {
    if (tex.pixels.empty() || tex.texture) continue;
    tex.texture = device_->createTexture(plume::RenderTextureDesc::Texture2D(tex.width, tex.height, 1, kUiTextureFormat));
    if (tex.texture) tex.view = tex.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kUiTextureFormat));
    if (tex.view) tex.descriptors = device_->createDescriptorSet(UiDescriptorSetDesc());
    if (!tex.descriptors) {
      REXGPU_ERROR("[gfx] overlay texture {} ({}x{}) could not be created", id, tex.width, tex.height);
      tex.pixels.clear();
      continue;
    }
    tex.descriptors->setTexture(0, tex.texture.get(), plume::RenderTextureLayout::SHADER_READ, tex.view.get());
    tex.descriptors->setSampler(1, ui_sampler_.get());
    const uint32_t pitch = (tex.width * 4 + kCopyRowAlign - 1) & ~(kCopyRowAlign - 1);
    staged = (staged + kCopyPlacementAlign - 1) & ~(kCopyPlacementAlign - 1);
    pending.push_back({&tex, staged, pitch});
    staged += pitch * tex.height;
  }
  if (pending.empty()) return;

  if (!ui_upload_[frame_slot_] || ui_upload_bytes_[frame_slot_] < staged) {
    ui_upload_[frame_slot_] = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(staged));
    ui_upload_bytes_[frame_slot_] = ui_upload_[frame_slot_] ? staged : 0;
  }
  uint8_t* mapped = ui_upload_[frame_slot_] ? static_cast<uint8_t*>(ui_upload_[frame_slot_]->map()) : nullptr;
  if (!mapped) {
    for (const PendingUpload& up : pending) up.texture->texture.reset();
    return;
  }
  for (const PendingUpload& up : pending) {
    const UiTexture& tex = *up.texture;
    for (uint32_t y = 0; y < tex.height; ++y) std::memcpy(mapped + up.offset + size_t(y) * up.pitch, tex.pixels.data() + size_t(y) * tex.width * 4, size_t(tex.width) * 4);
  }
  ui_upload_[frame_slot_]->unmap();
  for (const PendingUpload& up : pending) {
    UiTexture& tex = *up.texture;
    list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(tex.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    const auto src = plume::RenderTextureCopyLocation::PlacedFootprint(ui_upload_[frame_slot_].get(), kUiTextureFormat, tex.width, tex.height, 1, up.pitch / 4, up.offset);
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(tex.texture.get(), 0, 0), src);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(tex.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    tex.pixels.clear();
    tex.pixels.shrink_to_fit();
  }
}

void GraphicsSystem::DrawUi(plume::RenderCommandList* list, plume::RenderTexture* target, plume::RenderFramebuffer* framebuffer, uint32_t width, uint32_t height) {
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    if (ui_pending_fresh_) {
      ui_current_ = std::move(ui_pending_);
      ui_pending_ = UiFrame();
      ui_pending_fresh_ = false;
    }
  }
  const UiFrame& frame = ui_current_;
  if (frame.draws.empty() || frame.vertices.empty() || !EnsureUiPipeline()) return;
  UploadUiTextures(list);

  const uint64_t bytes = uint64_t(frame.vertices.size()) * sizeof(UiVertex);
  if (!ui_vertices_[frame_slot_] || ui_vertex_bytes_[frame_slot_] < bytes) {
    const uint64_t capacity = std::max<uint64_t>(bytes * 2, 64 * 1024);
    ui_vertices_[frame_slot_] = device_->createBuffer(plume::RenderBufferDesc::VertexBuffer(capacity, plume::RenderHeapType::UPLOAD));
    ui_vertex_bytes_[frame_slot_] = ui_vertices_[frame_slot_] ? capacity : 0;
    if (!ui_vertices_[frame_slot_]) return;
  }
  void* mapped = ui_vertices_[frame_slot_]->map();
  if (!mapped) return;
  std::memcpy(mapped, frame.vertices.data(), size_t(bytes));
  ui_vertices_[frame_slot_]->unmap();

  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
  list->setFramebuffer(framebuffer);
  list->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  list->setPipeline(ui_pipeline_.get());
  list->setGraphicsPipelineLayout(ui_layout_.get());
  const plume::RenderVertexBufferView view(ui_vertices_[frame_slot_].get(), uint32_t(bytes));
  const plume::RenderInputSlot slot(0, sizeof(UiVertex));
  list->setVertexBuffers(0, &view, 1, &slot);

  std::lock_guard<std::mutex> lock(ui_mutex_);
  for (const UiDraw& draw : frame.draws) {
    const auto found = ui_textures_.find(draw.texture_id);
    if (found == ui_textures_.end() || !found->second.descriptors || !found->second.pixels.empty()) continue;
    const int32_t x = std::clamp(draw.scissor_x, 0, int32_t(width));
    const int32_t y = std::clamp(draw.scissor_y, 0, int32_t(height));
    const int32_t w = std::clamp(draw.scissor_w, 0, int32_t(width) - x);
    const int32_t h = std::clamp(draw.scissor_h, 0, int32_t(height) - y);
    if (w <= 0 || h <= 0) continue;
    list->setScissors(plume::RenderRect(x, y, x + w, y + h));
    list->setGraphicsDescriptorSet(found->second.descriptors.get(), 0);
    list->drawInstanced(draw.vertex_count, 1, draw.first_vertex, 0);
  }
  list->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
}

void GraphicsSystem::ReleaseUi() {
  std::lock_guard<std::mutex> lock(ui_mutex_);
  ui_textures_.clear();
  ui_current_ = UiFrame();
  ui_pending_ = UiFrame();
  ui_pending_fresh_ = false;
  for (auto& buffer : ui_vertices_) buffer.reset();
  for (auto& buffer : ui_upload_) buffer.reset();
  std::fill(std::begin(ui_vertex_bytes_), std::end(ui_vertex_bytes_), 0);
  std::fill(std::begin(ui_upload_bytes_), std::end(ui_upload_bytes_), 0);
  ui_pipeline_.reset();
  ui_layout_.reset();
  ui_vs_.reset();
  ui_ps_.reset();
  ui_sampler_.reset();
}

}  // namespace cherry::gpu
