#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

#include <plume_render_interface.h>
#ifdef _WIN32
#include <plume_d3d12.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/debug/edram_alias.h"
#include "gpu/debug/frame_timing.h"
#include "gpu/debug/renderdoc.h"
#include "gpu/draw/draw_frame.h"
#include "gpu/draw/vertex_format.h"
#include "gpu/graphics_system.h"
#include "gpu/shaders/builtin_shaders.h"
#include "gpu/shaders/shader_cache.h"

REXCVAR_DECLARE(bool, cherry_gfx_fxaa);
REXCVAR_DECLARE(bool, cherry_gfx_stretch);
REXCVAR_DEFINE_INT32(cherry_debug_trace_frame, 0, "Lollipop/Debug","Log every command of this replayed frame number (as counted in the frame log): shaders, targets and each texture's address, format, size and heap slot.");

namespace cherry::gpu {

namespace {

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr plume::RenderFormat kPlaceholderFormat = plume::RenderFormat::R8G8B8A8_UNORM;
constexpr uint32_t kMaxTextures = 16384;
constexpr uint32_t kMaxSamplers = 256;
// The last sampler descriptor is the blit's linear clamp; the texture cache hands out the rest.
constexpr uint32_t kBlitSampler = kMaxSamplers - 1;
// D3D12 root constant buffer views must start on 256 bytes; texture copies from a buffer on 512.
constexpr uint32_t kConstantAlign = 256;
constexpr uint32_t kCopyPlacementAlign = 512;
constexpr uint32_t kCopyRowAlign = 256;
constexpr uint32_t kConstantBlockBytes = 4096;
constexpr uint32_t kSharedBlockBytes = 512;
// XenosRecomp's SPIR-V spec constant 0; DXIL bakes the same value (shader_common.h).
constexpr uint32_t kSpecConstantAlphaTest = 1u << 1;

// SharedConstants as the translated shaders read it: descriptor indices for 16 fetch slots per heap, then the shared scalars.
struct SharedConstants {
  uint32_t texture_2d[16];
  uint32_t texture_3d[16];
  uint32_t texture_cube[16];
  uint32_t sampler[16];
  uint32_t booleans;
  uint32_t swapped_texcoords;
  float half_pixel_offset[2];
  float alpha_threshold;
  uint32_t vertex_atlas;
  uint32_t vertex_atlas_base;
  uint32_t vertex_elements;
  int32_t vertex_index_base;
  uint32_t color_clamp;  // two bits per render target: 1 clamps to [0, 1], 2 to the 7e3 range (colorClamp in the patched shader_common.h)
  float depth_offset;  // PolyOffsetFrontOffset in NDC depth, added to oPos.z by the patched vertex shader epilogue
  float inv_resolution_scale;  // one over the render resolution scale: the patched pixel shaders bring their screen position back to guest pixels with it
  uint8_t vertex_slots[32];  // the host element slot of each vertex input location
  uint32_t vertex_texture_2d[kSamplerSlots - kPixelSamplers];  // samplers 16..19, for vertex texture fetch
  uint32_t vertex_sampler[kSamplerSlots - kPixelSamplers];
  uint32_t scaled_textures;  // DrawRecord::scaled_textures, for TEXEL_SCALE in the patched shader_common.h
};
static_assert(offsetof(SharedConstants, booleans) == 256 && offsetof(SharedConstants, alpha_threshold) == 272 && offsetof(SharedConstants, vertex_index_base) == 288 && offsetof(SharedConstants, depth_offset) == 296 && offsetof(SharedConstants, inv_resolution_scale) == 300 && offsetof(SharedConstants, vertex_slots) == 304 && offsetof(SharedConstants, vertex_texture_2d) == 336 && offsetof(SharedConstants, vertex_sampler) == 352 && offsetof(SharedConstants, scaled_textures) == 368);

// What blit.hlsl reads from the shared-constants slot.
struct BlitConstants {
  uint32_t texture;
  uint32_t sampler;
  uint32_t fxaa;  // 1 runs FXAA over the source as it is scaled onto the swap chain
  float padding;
  float source_rect[4];
};

// The viewport's depth scale, as the replay sets it: VportZScaleEnable bit 4 of GPU_VTECONTROL, else 1.
float ViewportDepthScale(const DrawRecord& record) {
  return (record.vte_control & 0x10) ? record.viewport[4] : 1.0f;
}

// D3D stores DepthBias as is, an offset in window depth; a host float depth buffer scales its integer bias per primitive, so the vertex shader adds it in NDC instead.
float NdcDepthOffset(const DrawRecord& record) {
  const float scale = ViewportDepthScale(record);
  return record.poly_offset != 0.0f && scale != 0.0f ? record.poly_offset / scale : 0.0f;
}

plume::RenderComparisonFunction XenosCompare(uint32_t function) {
  switch (function & 7) {
    case 0: return plume::RenderComparisonFunction::NEVER;
    case 1: return plume::RenderComparisonFunction::LESS;
    case 2: return plume::RenderComparisonFunction::EQUAL;
    case 3: return plume::RenderComparisonFunction::LESS_EQUAL;
    case 4: return plume::RenderComparisonFunction::GREATER;
    case 5: return plume::RenderComparisonFunction::NOT_EQUAL;
    case 6: return plume::RenderComparisonFunction::GREATER_EQUAL;
    default: return plume::RenderComparisonFunction::ALWAYS;
  }
}

// DepthControl stencil face at `shift`: function, then fail, pass and depth-fail ops 3 bits each, in plume's order offset by one.
plume::RenderStencilFaceDesc XenosStencilFace(uint32_t depth_control, uint32_t shift) {
  plume::RenderStencilFaceDesc face;
  face.compareFunction = XenosCompare(depth_control >> shift);
  face.failOp = plume::RenderStencilOp(((depth_control >> (shift + 3)) & 7) + 1);
  face.passOp = plume::RenderStencilOp(((depth_control >> (shift + 6)) & 7) + 1);
  face.depthFailOp = plume::RenderStencilOp(((depth_control >> (shift + 9)) & 7) + 1);
  return face;
}

// The translated pixel shaders clip when alpha < g_AlphaThreshold, so only "greater" alpha functions map exactly; ColorControl holds the function in bits 0..2 and the enable in bit 3.
// Every translated pixel shader clips at `alpha - threshold`; shaders write alpha far outside [0, 1] (UE3's depth of field puts a signed blur amount there), so pass-all is -infinity.
float AlphaThreshold(uint32_t color_control, float alpha_ref) {
  constexpr float kPassAll = -std::numeric_limits<float>::infinity();
  if (!(color_control & 0x8)) return kPassAll;
  switch (color_control & 7) {
    case 0: return std::numeric_limits<float>::infinity();
    case 4:
    case 6: return alpha_ref;
    default: return kPassAll;
  }
}

plume::RenderBlend XenosBlend(uint32_t factor) {
  switch (factor) {
    case 0: return plume::RenderBlend::ZERO;
    case 1: return plume::RenderBlend::ONE;
    case 4: return plume::RenderBlend::SRC_COLOR;
    case 5: return plume::RenderBlend::INV_SRC_COLOR;
    case 6: return plume::RenderBlend::SRC_ALPHA;
    case 7: return plume::RenderBlend::INV_SRC_ALPHA;
    case 8: return plume::RenderBlend::DEST_COLOR;
    case 9: return plume::RenderBlend::INV_DEST_COLOR;
    case 10: return plume::RenderBlend::DEST_ALPHA;
    case 11: return plume::RenderBlend::INV_DEST_ALPHA;
    case 12: case 14: return plume::RenderBlend::BLEND_FACTOR;
    case 13: case 15: return plume::RenderBlend::INV_BLEND_FACTOR;
    case 16: return plume::RenderBlend::SRC_ALPHA_SAT;
    default: return plume::RenderBlend::ONE;
  }
}

plume::RenderBlendOperation XenosBlendOp(uint32_t op) {
  switch (op) {
    case 1: return plume::RenderBlendOperation::SUBTRACT;
    case 2: return plume::RenderBlendOperation::MIN;
    case 3: return plume::RenderBlendOperation::MAX;
    case 4: return plume::RenderBlendOperation::REV_SUBTRACT;
    default: return plume::RenderBlendOperation::ADD;
  }
}

// BlendControl: colour src 0..4, op 5..7, dst 8..12; alpha src 16..20, op 21..23, dst 24..28. ONE/ZERO/ADD on both halves is blending off.
plume::RenderBlendDesc XenosBlendDesc(uint32_t control) {
  plume::RenderBlendDesc desc;
  desc.srcBlend = XenosBlend(control & 0x1F);
  desc.blendOp = XenosBlendOp((control >> 5) & 0x7);
  desc.dstBlend = XenosBlend((control >> 8) & 0x1F);
  desc.srcBlendAlpha = XenosBlend((control >> 16) & 0x1F);
  desc.blendOpAlpha = XenosBlendOp((control >> 21) & 0x7);
  desc.dstBlendAlpha = XenosBlend((control >> 24) & 0x1F);
  desc.blendEnabled = (control & 0x1FFF1FFF) != 0x00010001;
  return desc;
}

plume::RenderPrimitiveTopology HostTopology(DrawTopology topology) {
  switch (topology) {
    case DrawTopology::PointList: return plume::RenderPrimitiveTopology::POINT_LIST;
    case DrawTopology::LineList: return plume::RenderPrimitiveTopology::LINE_LIST;
    case DrawTopology::LineStrip: return plume::RenderPrimitiveTopology::LINE_STRIP;
    case DrawTopology::TriangleStrip: return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
    default: return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
}

}  // namespace

// The translated shaders dictate this layout: unbounded Texture2D, Texture3D and TextureCube arrays in spaces 0-2, samplers in space 3, three constant blocks in space 4.
bool GraphicsSystem::EnsureDrawResources(plume::RenderCommandList* list) {
  if (draw_ready_) return true;
  if (draw_failed_) return false;
  draw_failed_ = true;
  if (!device_->getCapabilities().descriptorIndexing) {
    REXGPU_ERROR("[gfx] the device has no descriptor indexing; the draw path is unavailable");
    return false;
  }

  const plume::RenderDescriptorRange tex2d(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange tex3d(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange cube(plume::RenderDescriptorRangeType::TEXTURE, 0, kMaxTextures);
  const plume::RenderDescriptorRange sampler(plume::RenderDescriptorRangeType::SAMPLER, 0, kMaxSamplers);
  // Set index is the HLSL space, so the order is load-bearing.
  const plume::RenderDescriptorSetDesc set_descs[] = {
      plume::RenderDescriptorSetDesc(&tex2d, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&tex3d, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&cube, 1, true, kMaxTextures),
      plume::RenderDescriptorSetDesc(&sampler, 1, true, kMaxSamplers),
  };
  const plume::RenderRootDescriptorDesc root_descs[] = {
      plume::RenderRootDescriptorDesc(0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
      plume::RenderRootDescriptorDesc(1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
      plume::RenderRootDescriptorDesc(2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER),
  };
  // The SPIR-V reads the blocks through vk::RawBufferLoad from three buffer addresses in push constants; plume's Vulkan backend has no root descriptors.
  const plume::RenderPushConstantRange push_ranges[] = {
      plume::RenderPushConstantRange(0, 4, 0, uint32_t(sizeof(uint64_t) * 3), plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL),
  };
  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = set_descs;
  layout_desc.descriptorSetDescsCount = uint32_t(std::size(set_descs));
  if (using_vulkan_) {
    layout_desc.pushConstantRanges = push_ranges;
    layout_desc.pushConstantRangesCount = uint32_t(std::size(push_ranges));
  } else {
    layout_desc.rootDescriptorDescs = root_descs;
    layout_desc.rootDescriptorDescsCount = uint32_t(std::size(root_descs));
  }
  layout_desc.allowInputLayout = true;
  draw_layout_ = device_->createPipelineLayout(layout_desc);
  for (size_t i = 0; i < std::size(draw_sets_); ++i) draw_sets_[i] = device_->createDescriptorSet(set_descs[i]);
  if (!draw_layout_ || !draw_sets_[0] || !draw_sets_[1] || !draw_sets_[2] || !draw_sets_[3]) {
    REXGPU_ERROR("[gfx] bindless layout creation failed");
    return false;
  }

  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    upload_[slot] = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(kUploadBytes, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE));
    upload_mapped_[slot] = upload_[slot] ? static_cast<uint8_t*>(upload_[slot]->map()) : nullptr;
    if (!upload_mapped_[slot]) {
      REXGPU_ERROR("[gfx] upload buffer creation failed");
      return false;
    }
  }

  // White placeholders at descriptor 0 of every heap until textures are translated.
  placeholder_textures_[0] = device_->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, kPlaceholderFormat));
  placeholder_textures_[1] = device_->createTexture(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, kPlaceholderFormat));
  placeholder_textures_[2] = device_->createTexture(plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6, kPlaceholderFormat, plume::RenderTextureFlag::CUBE));
  placeholder_upload_ = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(kCopyPlacementAlign * 8));
  placeholder_sampler_ = device_->createSampler(plume::RenderSamplerDesc());
  if (!placeholder_textures_[0] || !placeholder_textures_[1] || !placeholder_textures_[2] || !placeholder_upload_ || !placeholder_sampler_) {
    REXGPU_ERROR("[gfx] placeholder texture creation failed");
    return false;
  }
  placeholder_views_[0] = placeholder_textures_[0]->createTextureView(plume::RenderTextureViewDesc::Texture2D(kPlaceholderFormat));
  placeholder_views_[1] = placeholder_textures_[1]->createTextureView(plume::RenderTextureViewDesc::Texture3D(kPlaceholderFormat));
  placeholder_views_[2] = placeholder_textures_[2]->createTextureView(plume::RenderTextureViewDesc::TextureCube(kPlaceholderFormat));
  uint8_t* white = static_cast<uint8_t*>(placeholder_upload_->map());
  std::memset(white, 0xFF, kCopyPlacementAlign * 8);
  placeholder_upload_->unmap();

  for (int i = 0; i < 3; ++i) list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(placeholder_textures_[i].get(), plume::RenderTextureLayout::COPY_DEST));
  const uint32_t row_texels = kCopyRowAlign / 4;
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(placeholder_textures_[0].get()), plume::RenderTextureCopyLocation::PlacedFootprint(placeholder_upload_.get(), kPlaceholderFormat, 1, 1, 1, row_texels, 0));
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(placeholder_textures_[1].get()), plume::RenderTextureCopyLocation::PlacedFootprint(placeholder_upload_.get(), kPlaceholderFormat, 1, 1, 1, row_texels, kCopyPlacementAlign));
  for (uint32_t face = 0; face < 6; ++face) list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(placeholder_textures_[2].get(), 0, face), plume::RenderTextureCopyLocation::PlacedFootprint(placeholder_upload_.get(), kPlaceholderFormat, 1, 1, 1, row_texels, kCopyPlacementAlign * (2 + face)));
  for (int i = 0; i < 3; ++i) list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(placeholder_textures_[i].get(), plume::RenderTextureLayout::SHADER_READ));
  for (int i = 0; i < 3; ++i) draw_sets_[i]->setTexture(0, placeholder_textures_[i].get(), plume::RenderTextureLayout::SHADER_READ, placeholder_views_[i].get());
  draw_sets_[3]->setSampler(0, placeholder_sampler_.get());

  textures_.Initialize(device_.get(), draw_sets_[0].get(), draw_sets_[1].get(), draw_sets_[2].get(), draw_sets_[3].get(), kMaxTextures, kBlitSampler);
  plume::RenderSamplerDesc blit_sampler;
  blit_sampler.addressU = plume::RenderTextureAddressMode::CLAMP;
  blit_sampler.addressV = plume::RenderTextureAddressMode::CLAMP;
  blit_sampler.addressW = plume::RenderTextureAddressMode::CLAMP;
  blit_sampler_ = device_->createSampler(blit_sampler);
  if (blit_sampler_) draw_sets_[3]->setSampler(kBlitSampler, blit_sampler_.get());
  draw_failed_ = false;
  draw_ready_ = true;
  REXGPU_INFO("[gfx] draw path ready ({} texture descriptors, {} MB upload per frame)", kMaxTextures, kUploadBytes >> 20);
  return true;
}

float GraphicsSystem::DrawScale(const DrawRecord& record) const {
  return record.color.valid() ? TargetScale(record.color.width, record.color.height) : TargetScale(record.depth.width, record.depth.height);
}

plume::RenderPipeline* GraphicsSystem::DrawPipeline(const DrawRecord& record, uint32_t color_formats, uint32_t depth_format) {
  plume::RenderShader* vs = ShaderModule(record.vs);
  plume::RenderShader* ps = record.ps ? ShaderModule(record.ps) : nullptr;
  if (!vs || (record.ps && !ps)) return nullptr;
  const uint32_t blend_key = record.blend_control & 0x1FFF1FFF;
  // Depth enable, write and function, and the whole stencil state when stencil is on.
  const uint32_t depth_key = depth_format ? ((record.depth_control & 1) ? record.depth_control : record.depth_control & 0x76) : 0;
  const uint32_t stencil_key = depth_format && (record.depth_control & 1) ? record.stencil_ref_mask & 0xFFFFFF : 0;
  const uint32_t cull_key = record.mode_control & 7;
  std::array<uint32_t, 3> extra_blend_keys = {};
  for (uint32_t i = 0; i < 3; ++i) extra_blend_keys[i] = (color_formats >> (8 * (i + 1))) & 0xFF ? record.extra_blend_controls[i] & 0x1FFF1FFF : 0;
  auto [found, inserted] = pipelines_.try_emplace(std::tuple{static_cast<const void*>(vs), static_cast<const void*>(ps), record.layout->key, uint32_t(record.topology), blend_key, color_formats, depth_format, depth_key, cull_key, record.color_mask, stencil_key, extra_blend_keys, depth_format ? std::bit_cast<uint32_t>(record.poly_offset_scale * DrawScale(record)) : 0u});
  if (!inserted) return found->second.get();

  std::vector<plume::RenderInputElement> elements;
  for (size_t i = 0; i < record.layout->elements.size(); ++i) {
    const HostElement& e = record.layout->elements[i];
    elements.emplace_back(DeclUsageSemantic(e.usage), e.usage_index, e.location, e.integer ? plume::RenderFormat::R32G32B32A32_UINT : plume::RenderFormat::R32G32B32A32_FLOAT, 0, uint32_t(i * 16));
  }
  const plume::RenderInputSlot slot(0, record.layout->stride());
  const plume::RenderSpecConstant spec(0, kSpecConstantAlphaTest);

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = draw_layout_.get();
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.primitiveTopology = HostTopology(record.topology);
  // ModeControl: cull front bit 0, back bit 1, clockwise front bit 2. -fvk-invert-y keeps a SPIR-V triangle where D3D puts it, so neither backend flips the winding (Kameo measured this).
  const bool cull_front = record.mode_control & 1;
  const bool cull_back = record.mode_control & 2;
  desc.cullMode = cull_front == cull_back ? plume::RenderCullMode::NONE : cull_front ? plume::RenderCullMode::FRONT : plume::RenderCullMode::BACK;
  desc.frontFace = ((record.mode_control >> 2) & 1) ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  for (uint32_t i = 0; i < 4 && ((color_formats >> (8 * i)) & 0xFF); ++i) {
    desc.renderTargetFormat[i] = plume::RenderFormat((color_formats >> (8 * i)) & 0xFF);
    desc.renderTargetBlend[i] = XenosBlendDesc(i == 0 ? record.blend_control : record.extra_blend_controls[i - 1]);
    desc.renderTargetBlend[i].renderTargetWriteMask = uint8_t((record.color_mask >> (4 * i)) & 0xF);
    desc.renderTargetCount = i + 1;
  }
  if (depth_format) {
    desc.depthTargetFormat = plume::RenderFormat(depth_format);
    // DepthControl: z enable bit 1, z write bit 2, function bits 4..6.
    desc.depthEnabled = (record.depth_control >> 1) & 1;
    desc.depthWriteEnabled = desc.depthEnabled && ((record.depth_control >> 2) & 1);
    desc.depthFunction = XenosCompare(record.depth_control >> 4);
    // The guest's slope term is D3D's SlopeScaleDepthBias x16 in 1/16 units, so dividing gives the host's unitless slope factor; both apply in window depth, and a host pixel is 1/scale of a guest one, so the scale keeps the bias per guest pixel.
    desc.slopeScaledDepthBias = record.poly_offset_scale / 16.0f * DrawScale(record);
    // DepthControl: stencil enable bit 0, two-sided bit 7, front face from bit 8, back face from bit 20.
    if (record.depth_control & 1) {
      desc.stencilEnabled = true;
      desc.stencilReference = record.stencil_ref_mask & 0xFF;
      desc.stencilReadMask = (record.stencil_ref_mask >> 8) & 0xFF;
      desc.stencilWriteMask = (record.stencil_ref_mask >> 16) & 0xFF;
      desc.stencilFrontFace = XenosStencilFace(record.depth_control, 8);
      desc.stencilBackFace = (record.depth_control & 0x80) ? XenosStencilFace(record.depth_control, 20) : desc.stencilFrontFace;
    }
  }
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  desc.inputElements = elements.data();
  desc.inputElementsCount = uint32_t(elements.size());
  if (using_vulkan_) {
    desc.specConstants = &spec;
    desc.specConstantsCount = 1;
  }
  found->second = device_->createGraphicsPipeline(desc);
  debug::AddFrameCount(debug::FrameCount::Pipelines, 1);
#ifdef _WIN32
  if (found->second && !using_vulkan_) {
    auto* d3d12 = static_cast<plume::D3D12GraphicsPipeline*>(found->second.get());
    // plume returns a pipeline even when CreateGraphicsPipelineState fails, and setPipeline then crashes on its null PSO.
    if (!d3d12->d3d) found->second.reset();
    // plume's D3D12 backend applies stencilRef in setPipeline but never copies it from the desc.
    else d3d12->stencilRef = desc.stencilReference;
  }
#endif
  if (!found->second && pipeline_failures_++ < 8) REXGPU_WARN("[gfx] pipeline creation failed for VS {:016X} / PS {:016X}: colour formats {:08X}, depth format {}, {} inputs, topology {}, depth control {:08X}", record.vs->hash, record.ps ? record.ps->hash : 0, color_formats, depth_format, elements.size(), uint32_t(record.topology), record.depth_control);
  return found->second.get();
}

// Resolves every draw's textures before the render pass opens, since uploads are copies.
void GraphicsSystem::PrepareGeometry(plume::RenderCommandList* list, DrawFrame& frame, std::vector<std::shared_ptr<HostGeometry>>& keep) {
  struct Copy {
    HostGeometry* geometry;
    int64_t vertices;
    int64_t indices;
  };
  std::vector<Copy> copies;
  std::vector<plume::RenderBufferBarrier> barriers;
  for (DrawRecord& record : frame.draws) {
    if (!record.geometry) continue;
    HostGeometry& geometry = *record.geometry;
    if (geometry.vertex_buffer || geometry.vertices.empty()) continue;
    const uint64_t vertex_bytes = geometry.vertices.size();
    const uint64_t index_bytes = uint64_t(geometry.indices.size()) * 4;
    const int64_t vertices = ring_.Alloc(vertex_bytes, 16);
    const int64_t indices = vertices < 0 ? -1 : ring_.Alloc(index_bytes, 16);
    if (indices < 0) continue;
    auto vertex_buffer = device_->createBuffer(plume::RenderBufferDesc::VertexBuffer(vertex_bytes, plume::RenderHeapType::DEFAULT));
    auto index_buffer = device_->createBuffer(plume::RenderBufferDesc::IndexBuffer(index_bytes, plume::RenderHeapType::DEFAULT));
    if (!vertex_buffer || !index_buffer) continue;
    std::memcpy(ring_.mapped + vertices, geometry.vertices.data(), vertex_bytes);
    std::memcpy(ring_.mapped + indices, geometry.indices.data(), index_bytes);
    debug::AddFrameCount(debug::FrameCount::GeometryBytes, vertex_bytes + index_bytes);
    geometry.vertex_buffer = std::move(vertex_buffer);
    geometry.index_buffer = std::move(index_buffer);
    copies.push_back({&geometry, vertices, indices});
    barriers.emplace_back(geometry.vertex_buffer.get(), plume::RenderBufferAccess::WRITE);
    barriers.emplace_back(geometry.index_buffer.get(), plume::RenderBufferAccess::WRITE);
  }
  for (DrawRecord& record : frame.draws) {
    if (record.geometry && (keep.empty() || keep.back() != record.geometry)) keep.push_back(record.geometry);
  }
  if (copies.empty()) return;
  list->barriers(plume::RenderBarrierStage::COPY, barriers);
  for (const Copy& copy : copies) {
    list->copyBufferRegion(copy.geometry->vertex_buffer->at(0), ring_.buffer->at(uint64_t(copy.vertices)), copy.geometry->vertices.size());
    list->copyBufferRegion(copy.geometry->index_buffer->at(0), ring_.buffer->at(uint64_t(copy.indices)), uint64_t(copy.geometry->indices.size()) * 4);
  }
  for (plume::RenderBufferBarrier& barrier : barriers) barrier.accessBits = plume::RenderBufferAccess::READ;
  list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers);
  for (const Copy& copy : copies) {
    std::vector<uint8_t>().swap(copy.geometry->vertices);
    std::vector<uint32_t>().swap(copy.geometry->indices);
  }
}

void GraphicsSystem::PrepareVertexAtlas(plume::RenderCommandList* list, DrawFrame& frame) {
  constexpr uint32_t kAtlasWidth = 4096;
  constexpr uint32_t kAtlasRows = 512;
  constexpr uint32_t kTexelBytes = 16;
  const uint32_t slot = frame_slot_;
  uint32_t next_row = 0;
  bool transitioned = false;
  for (DrawRecord& record : frame.draws) {
    if (record.kind != CommandKind::Draw || !record.vs || !(record.vs->specConstantsMask & kShaderIndexedVertexFetch) || record.geometry) continue;
    const uint64_t bytes = uint64_t(record.vertex_count) * record.layout->stride();
    const uint32_t rows = uint32_t((bytes / kTexelBytes + kAtlasWidth - 1) / kAtlasWidth);
    if (!rows || next_row + rows > kAtlasRows) continue;
    if (!vertex_atlas_[slot]) {
      vertex_atlas_[slot] = device_->createTexture(plume::RenderTextureDesc::Texture2D(kAtlasWidth, kAtlasRows, 1, plume::RenderFormat::R32G32B32A32_FLOAT));
      if (!vertex_atlas_[slot]) return;
      vertex_atlas_view_[slot] = vertex_atlas_[slot]->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R32G32B32A32_FLOAT));
      vertex_atlas_index_[slot] = textures_.ReserveIndex();
      draw_sets_[0]->setTexture(vertex_atlas_index_[slot], vertex_atlas_[slot].get(), plume::RenderTextureLayout::SHADER_READ, vertex_atlas_view_[slot].get());
    }
    const uint64_t pitch = uint64_t(kAtlasWidth) * kTexelBytes;
    const int64_t offset = ring_.Alloc(pitch * rows, kCopyPlacementAlign);
    if (offset < 0) continue;
    std::memcpy(ring_.mapped + offset, frame.arena.data() + record.vertices, bytes);
    std::memset(ring_.mapped + offset + bytes, 0, pitch * rows - bytes);
    if (!transitioned) {
      list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(vertex_atlas_[slot].get(), plume::RenderTextureLayout::COPY_DEST));
      transitioned = true;
    }
    auto source = plume::RenderTextureCopyLocation::PlacedFootprint(ring_.buffer, plume::RenderFormat::R32G32B32A32_FLOAT, kAtlasWidth, rows, 1, kAtlasWidth, uint64_t(offset));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(vertex_atlas_[slot].get(), 0), source, 0, next_row, 0);
    record.vertex_atlas_base = next_row * kAtlasWidth;
    next_row += rows;
  }
  if (transitioned) list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(vertex_atlas_[slot].get(), plume::RenderTextureLayout::SHADER_READ));
}

void GraphicsSystem::PrepareTextures(plume::RenderCommandList* list, DrawFrame& frame) {
  for (DrawRecord& record : frame.draws) {
    // Vertex texture fetch samples 2D textures only; the translator declares no other dimension for samplers 16..19.
    for (uint32_t s = kPixelSamplers; s < kSamplerSlots; ++s) {
      if (!(record.texture_mask & (1u << s))) continue;
      record.texture_index[s] = textures_.Texture2D(list, ring_, record.textures[s]);
      record.sampler_index[s] = textures_.Sampler(record.textures[s]);
    }
    if (!record.ps) continue;
    for (uint32_t s = 0; s < kPixelSamplers; ++s) {
      if (!(record.texture_mask & (1u << s))) continue;
      // Two bits per slot from the translator: 0 2D, 1 3D, 2 cube.
      const uint32_t dimension = (record.ps->textureDimensionMask >> (2 * s)) & 3;
      if (dimension == 0) {
        record.texture_index[s] = textures_.Texture2D(list, ring_, record.textures[s]);
        if (textures_.ResolvedScale(record.textures[s]) != 1.0f) record.scaled_textures |= 1u << s;
      }
      else if (dimension == 1) record.texture_3d_index[s] = textures_.Texture3D(list, ring_, record.textures[s]);
      else if (dimension == 2) record.texture_cube_index[s] = textures_.TextureCube(list, ring_, record.textures[s]);
      else continue;
      record.sampler_index[s] = textures_.Sampler(record.textures[s]);
    }
  }
}

void GraphicsSystem::ReplayFrame(plume::RenderCommandList* list, DrawFrame& frame) {
  if (frame.draws.empty() || !draw_ready_) return;
  debug::TrackEdram(frame);
  if (const uint64_t hash = debug::RenderDocTriggerShader()) {
    if (std::any_of(frame.draws.begin(), frame.draws.end(), [&](const DrawRecord& r) { return r.kind == CommandKind::Draw && r.vs && r.vs->hash == hash; })) debug::RenderDocShaderSeen(frame_index_);
  }
  static uint64_t replayed_frames = 0;
  const bool trace = REXCVAR_GET(cherry_debug_trace_frame) > 0 && uint64_t(REXCVAR_GET(cherry_debug_trace_frame)) == ++replayed_frames;
  if (trace) {
    uint32_t index = 0;
    for (const DrawRecord& r : frame.draws) {
      ++index;
      if (r.kind == CommandKind::Clear) {
        REXGPU_INFO("[trace] {} clear flags {:02X} colour {}x{} {:08X} depth {}x{} colour value ({}, {}, {}, {}) z {}", index, r.clear_flags, r.color.width, r.color.height, r.color.info, r.depth.width, r.depth.height, r.clear_color[0], r.clear_color[1], r.clear_color[2], r.clear_color[3], r.clear_depth);
      } else if (r.kind == CommandKind::Resolve) {
        REXGPU_INFO("[trace] {} resolve flags {:03X} source {}x{} {:08X} rect ({},{})-({},{}) -> {:08X} format {} {}x{}x{} at ({},{}) slice {}", index, r.resolve_flags, r.resolve_source.width, r.resolve_source.height, r.resolve_source.info, r.resolve_rect[0], r.resolve_rect[1], r.resolve_rect[2], r.resolve_rect[3], r.resolve_dest.address(), r.resolve_dest.format(), r.resolve_dest.width(), r.resolve_dest.height(), r.resolve_dest.depth(), r.resolve_point[0], r.resolve_point[1], r.resolve_slice);
      } else {
        REXGPU_INFO("[trace] {} draw VS {:016X} PS {:016X} target {}x{} {:08X} depth {}x{} {} indices, blend {:08X} depth {:08X} colour control {:08X} mode {:08X} mask {:X} stencil {:08X} scissor ({},{})-({},{})", index, r.vs->hash, r.ps ? r.ps->hash : 0, r.color.width, r.color.height, r.color.info, r.depth.width, r.depth.height, r.index_count, r.blend_control, r.depth_control, r.color_control, r.mode_control, r.color_mask, r.stencil_ref_mask, r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3]);
        for (uint32_t t = 0; t < 16; ++t) {
          if (!(r.texture_mask & (1u << t))) continue;
          REXGPU_INFO("[trace]     s{} {:08X} signs {:02X} format {} exp adjust {} {}x{}x{} {} fetch dimension {} shader dimension {} -> heap {} / 3D {} sampler {}", t, r.textures[t].address(), (r.textures[t].words[0] >> 2) & 0xFF, r.textures[t].format(), int32_t(r.textures[t].words[3] << 13) >> 26, r.textures[t].width(), r.textures[t].height(), r.textures[t].depth(), r.textures[t].tiled() ? "tiled" : "linear", r.textures[t].dimension(), ((r.ps ? r.ps->textureDimensionMask : 0) >> (2 * t)) & 3, r.texture_index[t], r.texture_3d_index[t], r.sampler_index[t]);
        }
        for (const HostElement& e : r.layout->elements) REXGPU_INFO("[trace]     in {} {} location {} stream {} offset {} type {:08X}", DeclUsageSemantic(e.usage), e.usage_index, e.location, e.guest_stream, e.guest_offset, e.guest_type);
        const float* vertices = reinterpret_cast<const float*>(r.geometry ? r.geometry->vertices.data() : frame.arena.data() + r.vertices);
        const uint32_t stride = r.layout->stride() / 4;
        for (uint32_t v = 0; vertices && v < std::min(r.vertex_count, 3u); ++v) REXGPU_INFO("[trace]     v{} element 0 ({}, {}, {}, {})", v, vertices[v * stride], vertices[v * stride + 1], vertices[v * stride + 2], vertices[v * stride + 3]);
        const float* vs_constants = reinterpret_cast<const float*>(frame.arena.data() + r.constants);
        for (uint32_t c = 6; c < 10; ++c) REXGPU_INFO("[trace]     c{} ({}, {}, {}, {})", c, vs_constants[4 * c], vs_constants[4 * c + 1], vs_constants[4 * c + 2], vs_constants[4 * c + 3]);
      }
    }
  }
  plume::RenderBuffer* upload = ring_.buffer;
  auto stage = [&](const void* data, uint64_t bytes, uint64_t alignment) -> int64_t {
    const int64_t offset = ring_.Alloc(bytes, alignment);
    if (offset >= 0 && data) std::memcpy(ring_.mapped + offset, data, bytes);
    return offset;
  };

  plume::RenderFramebuffer* bound = nullptr;
  // Transitions end the render pass, so anything that leaves a target out of its attachment layout forgets the binding.
  auto bind = [&](const ColorTargets& colors, HostTarget* depth) -> bool {
    plume::RenderFramebuffer* framebuffer = TargetFramebuffer(colors, depth);
    if (!framebuffer) return false;
    bool in_layout = !depth || depth->layout == uint32_t(plume::RenderTextureLayout::DEPTH_WRITE);
    for (HostTarget* color : colors) in_layout = in_layout && (!color || color->layout == uint32_t(plume::RenderTextureLayout::COLOR_WRITE));
    if (framebuffer != bound || !in_layout) {
      for (HostTarget* color : colors) TransitionTarget(list, color, uint32_t(plume::RenderTextureLayout::COLOR_WRITE));
      TransitionTarget(list, depth, uint32_t(plume::RenderTextureLayout::DEPTH_WRITE));
      list->setFramebuffer(framebuffer);
      list->setGraphicsPipelineLayout(draw_layout_.get());
      for (uint32_t i = 0; i < 4; ++i) list->setGraphicsDescriptorSet(draw_sets_[i].get(), i);
      bound = framebuffer;
    }
    const uint32_t width = colors[0] ? colors[0]->host_width : depth->host_width;
    const uint32_t height = colors[0] ? colors[0]->host_height : depth->host_height;
    list->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
    return true;
  };

  SharedConstants shared = {};
  uint32_t drawn = 0, cleared = 0, resolved = 0, skipped = 0, no_pipeline = 0, no_space = 0;
  for (const DrawRecord& record : frame.draws) {
    if (record.kind == CommandKind::Clear) {
      const ColorTargets colors = (record.clear_flags & 0xF) ? DrawTargets(record) : ColorTargets{};
      HostTarget* depth = (record.clear_flags & 0x30) ? Target(record.depth, true) : nullptr;
      if ((!colors[0] && !depth) || !bind(colors, depth)) {
        ++skipped;
        continue;
      }
      const float s = colors[0] ? colors[0]->scale : depth->scale;
      const plume::RenderRect rect(ScaleCoord(record.clear_rect[0], s), ScaleCoord(record.clear_rect[1], s), ScaleCoord(record.clear_rect[2], s), ScaleCoord(record.clear_rect[3], s));
      const plume::RenderRect* rects = record.clear_has_rect ? &rect : nullptr;
      // D3DCLEAR_TARGET0..3 are flags bits 0..3.
      for (uint32_t i = 0; i < 4 && colors[i]; ++i) {
        if (record.clear_flags & (1u << i)) colors[i]->last_write = ++target_writes_;
        if (record.clear_flags & (1u << i)) list->clearColor(i, plume::RenderColor(record.clear_color[0], record.clear_color[1], record.clear_color[2], record.clear_color[3]), rects, rects ? 1 : 0);
      }
      if (depth) list->clearDepthStencil((record.clear_flags & 0x10) != 0, (record.clear_flags & 0x20) != 0, record.clear_depth, 0, rects, rects ? 1 : 0);
      ++cleared;
      continue;
    }

    if (record.kind == CommandKind::Resolve && (record.resolve_flags & 7) == 4) {
      HostTarget* depth = Target(record.resolve_source, true);
      bound = nullptr;
      if (!depth || !ResolveDepth(list, record, depth)) {
        ++skipped;
        continue;
      }
      if ((record.resolve_flags & 0x200) && bind({}, depth)) list->clearDepthStencil(true, true, record.resolve_clear_depth, 0);
      ++resolved;
      continue;
    }

    if (record.kind == CommandKind::Resolve) {
      HostTarget* source = Target(record.resolve_source, false);
      // GPU_COLORINFO.ColorExpBias scaled what the source holds and CopyDestExpBias (flags bits 26..31) scales the copy, each a signed power of two.
      const int32_t exp_bias = (int32_t(record.resolve_source.info << 6) >> 26) + (int32_t(record.resolve_flags) >> 26);
      // UE3 downsamples the scene by resolving a 4x MSAA surface laid over its tiles; the samples are the scene's own pixels.
      uint32_t samples_x = 1, samples_y = 1;
      if (HostTarget* alias = MsaaAlias(record.resolve_source, &samples_x, &samples_y); alias && (!source || alias->last_write > source->last_write)) {
        source = alias;
      } else {
        samples_x = samples_y = 1;
      }
      if (source && (exp_bias != 0 || samples_x * samples_y > 1) && record.resolve_dest.depth() == 1) {
        bound = nullptr;
        if (!ResolveColorScaled(list, record, source, std::ldexp(1.0f, exp_bias), samples_x, samples_y)) {
          ++skipped;
          continue;
        }
        if ((record.resolve_flags & 0x100) && bind({source}, nullptr)) list->clearColor(0, plume::RenderColor(record.resolve_clear_color[0], record.resolve_clear_color[1], record.resolve_clear_color[2], record.resolve_clear_color[3]));
        if (record.resolve_flags & 0x200) {
          if (HostTarget* depth = Target(record.depth, true); depth && bind({}, depth)) list->clearDepthStencil(true, true, record.resolve_clear_depth, 0);
        }
        ++resolved;
        continue;
      }
      plume::RenderTexture* dest = source ? textures_.BeginResolve(list, record.resolve_dest, source->format, source->scale) : nullptr;
      if (!dest) {
        static uint32_t reported = 0;
        if (reported++ < 24) REXGPU_INFO("[gfx] resolve skipped: flags {:08X}, source {}x{} info {:08X}, destination {:08X} format {} {}x{}", record.resolve_flags, record.resolve_source.width, record.resolve_source.height, record.resolve_source.info, record.resolve_dest.address(), record.resolve_dest.format(), record.resolve_dest.width(), record.resolve_dest.height());
        ++skipped;
        continue;
      }
      TransitionTarget(list, source, uint32_t(plume::RenderTextureLayout::COPY_SOURCE));
      bound = nullptr;
      const int32_t dest_w = int32_t(record.resolve_dest.width());
      const int32_t dest_h = int32_t(record.resolve_dest.height());
      const int32_t dx = std::clamp(record.resolve_point[0], 0, dest_w);
      const int32_t dy = std::clamp(record.resolve_point[1], 0, dest_h);
      const int32_t left = std::clamp(record.resolve_rect[0], 0, int32_t(source->width));
      const int32_t top = std::clamp(record.resolve_rect[1], 0, int32_t(source->height));
      const int32_t right = std::clamp(std::min(record.resolve_rect[2], left + dest_w - dx), left, int32_t(source->width));
      const int32_t bottom = std::clamp(std::min(record.resolve_rect[3], top + dest_h - dy), top, int32_t(source->height));
      if (right > left && bottom > top) {
        // Worked out in guest pixels, then scaled: the destination is at the source's scale.
        const float s = source->scale;
        const int32_t host_dx = ScaleCoord(dx, s);
        const int32_t host_dy = ScaleCoord(dy, s);
        const int32_t host_left = ScaleCoord(left, s);
        const int32_t host_top = ScaleCoord(top, s);
        const int32_t host_right = std::min({ScaleCoord(right, s), int32_t(source->host_width), host_left + int32_t(TextureCache::Scaled(uint32_t(dest_w), s)) - host_dx});
        const int32_t host_bottom = std::min({ScaleCoord(bottom, s), int32_t(source->host_height), host_top + int32_t(TextureCache::Scaled(uint32_t(dest_h), s)) - host_dy});
        const plume::RenderBox box(host_left, host_top, host_right, host_bottom);
        const uint32_t dz = std::min(record.resolve_slice, record.resolve_dest.depth() - 1);
        if (host_right > host_left && host_bottom > host_top) list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(dest), plume::RenderTextureCopyLocation::Subresource(source->texture.get()), uint32_t(host_dx), uint32_t(host_dy), dz, &box);
      }
      textures_.EndResolve(list, record.resolve_dest);
      // D3DRESOLVE_CLEARRENDERTARGET (0x100) and D3DRESOLVE_CLEARDEPTHSTENCIL (0x200) clear the source after the copy.
      if ((record.resolve_flags & 0x100) && bind({source}, nullptr)) list->clearColor(0, plume::RenderColor(record.resolve_clear_color[0], record.resolve_clear_color[1], record.resolve_clear_color[2], record.resolve_clear_color[3]));
      if (record.resolve_flags & 0x200) {
        if (HostTarget* depth = Target(record.depth, true); depth && bind({}, depth)) list->clearDepthStencil(true, true, record.resolve_clear_depth, 0);
      }
      ++resolved;
      continue;
    }

    // A draw with only a depth target (colour mask 0, no surface bound) writes depth alone, as a pre-pass does.
    const ColorTargets colors = DrawTargets(record);
    HostTarget* color = colors[0];
    HostTarget* depth = Target(record.depth, true);
    if ((!color && !depth) || !bind(colors, depth)) {
      ++skipped;
      continue;
    }
    uint32_t color_formats = 0;
    for (uint32_t i = 0; i < 4 && colors[i]; ++i) color_formats |= (colors[i]->format & 0xFF) << (8 * i);
    plume::RenderPipeline* pipeline = DrawPipeline(record, color_formats, depth ? depth->format : 0);
    if (!pipeline) {
      ++no_pipeline;
      continue;
    }
    shared.booleans = record.booleans;
    shared.vertex_index_base = record.vertex_index_base;
    shared.color_clamp = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      // GPU_COLORINFO.ColorFormat: 2_10_10_10 (2, and 10 as 10_10_10_10) is unsigned normalised, 2_10_10_10_FLOAT (3, and 12 as 16_16_16_16) is 7e3.
      const uint32_t format = ((i == 0 ? record.color.info : record.extra_colors[i - 1].info) >> 16) & 0xF;
      shared.color_clamp |= (format == 2 || format == 10 ? 1u : format == 3 || format == 12 ? 2u : 0u) << (2 * i);
    }
    shared.vertex_atlas = record.vertex_atlas_base != ~0u ? vertex_atlas_index_[frame_slot_] : 0;
    shared.vertex_atlas_base = record.vertex_atlas_base != ~0u ? record.vertex_atlas_base : 0;
    shared.vertex_elements = uint32_t(record.layout->elements.size());
    std::memset(shared.vertex_slots, 0, sizeof(shared.vertex_slots));
    for (size_t e = 0; e < record.layout->elements.size(); ++e) {
      if (record.layout->elements[e].location < 32) shared.vertex_slots[record.layout->elements[e].location] = uint8_t(e);
    }
    shared.alpha_threshold = AlphaThreshold(record.color_control, record.alpha_ref);
    shared.depth_offset = NdcDepthOffset(record);
    shared.inv_resolution_scale = 1.0f / (color ? color : depth)->scale;
    // The patched shader_common.h's TEXEL_SCALE: a bit per sampler, and above them the scale resolves are at, in 1/4096 units.
    shared.scaled_textures = record.scaled_textures | (uint32_t(std::lround(resolution_scale_ * 4096.0f)) << 16);
    std::memcpy(shared.texture_2d, record.texture_index, sizeof(shared.texture_2d));
    std::memcpy(shared.texture_3d, record.texture_3d_index, sizeof(shared.texture_3d));
    std::memcpy(shared.texture_cube, record.texture_cube_index, sizeof(shared.texture_cube));
    std::memcpy(shared.sampler, record.sampler_index, sizeof(shared.sampler));
    std::memcpy(shared.vertex_texture_2d, record.texture_index + kPixelSamplers, sizeof(shared.vertex_texture_2d));
    std::memcpy(shared.vertex_sampler, record.sampler_index + kPixelSamplers, sizeof(shared.vertex_sampler));
    const uint8_t* arena = frame.arena.data();
    const int64_t vs = stage(arena + record.constants, kConstantBlockBytes, kConstantAlign);
    const int64_t ps = stage(arena + record.constants + kConstantBlockBytes, kConstantBlockBytes, kConstantAlign);
    const int64_t sh = stage(&shared, sizeof(shared), kConstantAlign);
    stage(nullptr, kSharedBlockBytes - sizeof(shared), 1);
    const HostGeometry* geometry = record.geometry.get();
    const uint64_t vertex_bytes = uint64_t(record.vertex_count) * record.layout->stride();
    const int64_t vb = geometry ? 0 : stage(arena + record.vertices, vertex_bytes, 16);
    const int64_t ib = geometry ? 0 : stage(arena + record.indices, uint64_t(record.index_count) * 4, 16);
    if (vs < 0 || ps < 0 || sh < 0 || vb < 0 || ib < 0 || (geometry && !geometry->vertex_buffer)) {
      ++no_space;
      continue;
    }

    list->setPipeline(pipeline);
    if (using_vulkan_) {
      const uint64_t address = upload->getDeviceAddress();
      const uint64_t push[3] = {address + uint64_t(vs), address + uint64_t(ps), address + uint64_t(sh)};
      list->setGraphicsPushConstants(0, push, 0, uint32_t(sizeof(push)));
    } else {
      list->setGraphicsRootDescriptor(upload->at(uint64_t(vs)), 0);
      list->setGraphicsRootDescriptor(upload->at(uint64_t(ps)), 1);
      list->setGraphicsRootDescriptor(upload->at(uint64_t(sh)), 2);
    }
    const plume::RenderVertexBufferView vertex_view(geometry ? geometry->vertex_buffer->at(0) : upload->at(uint64_t(vb)), uint32_t(vertex_bytes));
    const plume::RenderInputSlot slot(0, record.layout->stride());
    list->setVertexBuffers(0, &vertex_view, 1, &slot);
    const plume::RenderIndexBufferView index_view(geometry ? geometry->index_buffer->at(0) : upload->at(uint64_t(ib)), record.index_count * 4, plume::RenderFormat::R32_UINT);
    list->setIndexBuffer(&index_view);

    // The guest viewport, in its render target's own pixels.
    HostTarget* sized = color ? color : depth;
    float vx = 0.0f, vy = 0.0f, vw = float(sized->width), vh = float(sized->height), min_z = 0.0f, max_z = 1.0f;
    if (record.viewport[0] != 0.0f && record.viewport[2] != 0.0f) {
      vw = 2.0f * std::fabs(record.viewport[0]);
      vx = record.viewport[1] - std::fabs(record.viewport[0]);
      vh = 2.0f * std::fabs(record.viewport[2]);
      vy = record.viewport[3] - std::fabs(record.viewport[2]);
    }
    // The game's D3D initialises GPU_VTXCONTROL.PixCenter to GPUPIXCENTER_ZERO (sub_82A00E20), whose pixel centres sit half a pixel before the host's.
    if (!(record.vtx_control & 1)) {
      vx += 0.5f;
      vy += 0.5f;
    }
    // VportZScaleEnable bit 4, VportZOffsetEnable bit 5; UE3 maps depth reversed (scale -1, offset 1), which the host viewport keeps as min > max.
    const float z_scale = ViewportDepthScale(record);
    const float z_offset = (record.vte_control & 0x20) ? record.viewport[5] : 0.0f;
    min_z = std::clamp(z_offset, 0.0f, 1.0f);
    max_z = std::clamp(z_offset + z_scale, 0.0f, 1.0f);
    // Guest pixels to the render resolution; the half-pixel shift above scales with them, so a guest pixel's centre lands in the middle of its host pixels.
    const float s = sized->scale;
    list->setViewports(plume::RenderViewport(vx * s, vy * s, vw * s, vh * s, min_z, max_z));
    const int32_t target_w = int32_t(sized->width);
    const int32_t target_h = int32_t(sized->height);
    const int32_t left = std::clamp(record.scissor[0], 0, target_w);
    const int32_t top = std::clamp(record.scissor[1], 0, target_h);
    const int32_t right = std::clamp(record.scissor[2], left, target_w);
    const int32_t bottom = std::clamp(record.scissor[3], top, target_h);
    list->setScissors(plume::RenderRect(ScaleCoord(left, s), ScaleCoord(top, s), std::min(ScaleCoord(right, s), int32_t(sized->host_width)), std::min(ScaleCoord(bottom, s), int32_t(sized->host_height))));
    for (uint32_t i = 0; i < 4 && colors[i]; ++i) {
      if ((record.color_mask >> (4 * i)) & 0xF) colors[i]->last_write = ++target_writes_;
    }
    list->drawIndexedInstanced(record.index_count, 1, 0, 0, 0);
    ++drawn;
  }

  static uint64_t frames = 0;
  if ((frames++ % 300) == 0) REXGPU_INFO("[gfx] frame {}: {} commands; {} draws, {} clears, {} resolves replayed; {} skipped, {} without pipeline, {} out of upload space; captured drops: {} without shader, {} without layout, {} other ({} KB uploaded)", frames, frame.draws.size(), drawn, cleared, resolved, skipped, no_pipeline, no_space, frame.dropped_no_shader, frame.dropped_no_layout, frame.dropped_other, ring_.used >> 10);
}

bool GraphicsSystem::ResolveDepth(plume::RenderCommandList* list, const DrawRecord& record, HostTarget* depth) {
  if (!depth->heap_index) return false;
  if (!depth_resolve_pipeline_) {
    const plume::RenderShaderFormat format = using_vulkan_ ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL;
    depth_resolve_vs_ = using_vulkan_ ? device_->createShader(shaders::g_depthResolveVSSpirv, sizeof(shaders::g_depthResolveVSSpirv), "VSMain", format) : device_->createShader(shaders::g_depthResolveVSDxil, sizeof(shaders::g_depthResolveVSDxil), "VSMain", format);
    depth_resolve_ps_ = using_vulkan_ ? device_->createShader(shaders::g_depthResolvePSSpirv, sizeof(shaders::g_depthResolvePSSpirv), "PSMain", format) : device_->createShader(shaders::g_depthResolvePSDxil, sizeof(shaders::g_depthResolvePSDxil), "PSMain", format);
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = draw_layout_.get();
    desc.vertexShader = depth_resolve_vs_.get();
    desc.pixelShader = depth_resolve_ps_.get();
    desc.renderTargetFormat[0] = plume::RenderFormat::R32_FLOAT;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    depth_resolve_pipeline_ = device_->createGraphicsPipeline(desc);
    if (!depth_resolve_pipeline_) {
      REXGPU_ERROR("[gfx] depth resolve pipeline creation failed");
      return false;
    }
  }
  plume::RenderFramebuffer* framebuffer = nullptr;
  TransitionTarget(list, depth, uint32_t(plume::RenderTextureLayout::DEPTH_READ));
  const float s = depth->scale;
  if (!textures_.BeginResolve(list, record.resolve_dest, uint32_t(plume::RenderFormat::R32_FLOAT), s, &framebuffer) || !framebuffer) return false;
  struct {
    uint32_t texture;
    uint32_t padding;
    int32_t origin[2];
  } constants = {depth->heap_index, 0, {ScaleCoord(record.resolve_rect[0] - record.resolve_point[0], s), ScaleCoord(record.resolve_rect[1] - record.resolve_point[1], s)}};
  const int64_t offset = ring_.Alloc(sizeof(constants), kConstantAlign);
  if (offset < 0) return false;
  std::memcpy(ring_.mapped + offset, &constants, sizeof(constants));
  const int32_t dest_w = int32_t(record.resolve_dest.width());
  const int32_t dest_h = int32_t(record.resolve_dest.height());
  int32_t x = std::clamp(record.resolve_point[0], 0, dest_w);
  int32_t y = std::clamp(record.resolve_point[1], 0, dest_h);
  const int32_t w = ScaleCoord(std::clamp(record.resolve_rect[2] - record.resolve_rect[0], 0, dest_w - x), s);
  const int32_t h = ScaleCoord(std::clamp(record.resolve_rect[3] - record.resolve_rect[1], 0, dest_h - y), s);
  x = ScaleCoord(x, s);
  y = ScaleCoord(y, s);
  list->setFramebuffer(framebuffer);
  list->setGraphicsPipelineLayout(draw_layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list->setGraphicsDescriptorSet(draw_sets_[i].get(), i);
  list->setPipeline(depth_resolve_pipeline_.get());
  if (using_vulkan_) {
    const uint64_t address = ring_.buffer->getDeviceAddress() + uint64_t(offset);
    const uint64_t push[3] = {address, address, address};
    list->setGraphicsPushConstants(0, push, 0, uint32_t(sizeof(push)));
  } else {
    for (uint32_t i = 0; i < 3; ++i) list->setGraphicsRootDescriptor(ring_.buffer->at(uint64_t(offset)), i);
  }
  list->setViewports(plume::RenderViewport(float(x), float(y), float(w), float(h)));
  list->setScissors(plume::RenderRect(x, y, x + w, y + h));
  list->drawInstanced(3, 1, 0, 0);
  textures_.EndResolve(list, record.resolve_dest);
  return true;
}

bool GraphicsSystem::ResolveColorScaled(plume::RenderCommandList* list, const DrawRecord& record, HostTarget* source, float scale, uint32_t samples_x, uint32_t samples_y) {
  if (!source->heap_index) return false;
  if (!color_resolve_vs_) {
    const plume::RenderShaderFormat format = using_vulkan_ ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL;
    color_resolve_vs_ = using_vulkan_ ? device_->createShader(shaders::g_colorResolveVSSpirv, sizeof(shaders::g_colorResolveVSSpirv), "VSMain", format) : device_->createShader(shaders::g_colorResolveVSDxil, sizeof(shaders::g_colorResolveVSDxil), "VSMain", format);
    color_resolve_ps_ = using_vulkan_ ? device_->createShader(shaders::g_colorResolvePSSpirv, sizeof(shaders::g_colorResolvePSSpirv), "PSMain", format) : device_->createShader(shaders::g_colorResolvePSDxil, sizeof(shaders::g_colorResolvePSDxil), "PSMain", format);
  }
  auto& pipeline = color_resolve_pipelines_[source->format];
  if (!pipeline) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = draw_layout_.get();
    desc.vertexShader = color_resolve_vs_.get();
    desc.pixelShader = color_resolve_ps_.get();
    desc.renderTargetFormat[0] = plume::RenderFormat(source->format);
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = device_->createGraphicsPipeline(desc);
#ifdef _WIN32
    if (pipeline && !using_vulkan_ && !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) pipeline.reset();
#endif
    if (!pipeline) {
      REXGPU_ERROR("[gfx] colour resolve pipeline creation failed for format {}", source->format);
      return false;
    }
  }
  plume::RenderFramebuffer* framebuffer = nullptr;
  TransitionTarget(list, source, uint32_t(plume::RenderTextureLayout::SHADER_READ));
  const float s = source->scale;
  if (!textures_.BeginResolve(list, record.resolve_dest, source->format, s, &framebuffer) || !framebuffer) return false;
  struct {
    uint32_t texture;
    float scale;
    int32_t origin[2];
    uint32_t samples[2];
  } constants = {source->heap_index, scale, {ScaleCoord(record.resolve_rect[0] - record.resolve_point[0], s), ScaleCoord(record.resolve_rect[1] - record.resolve_point[1], s)}, {samples_x, samples_y}};
  const int64_t offset = ring_.Alloc(sizeof(constants), kConstantAlign);
  if (offset < 0) return false;
  std::memcpy(ring_.mapped + offset, &constants, sizeof(constants));
  const int32_t dest_w = int32_t(record.resolve_dest.width());
  const int32_t dest_h = int32_t(record.resolve_dest.height());
  int32_t x = std::clamp(record.resolve_point[0], 0, dest_w);
  int32_t y = std::clamp(record.resolve_point[1], 0, dest_h);
  const int32_t w = ScaleCoord(std::clamp(std::min(record.resolve_rect[2], int32_t(source->width / samples_x)) - record.resolve_rect[0], 0, dest_w - x), s);
  const int32_t h = ScaleCoord(std::clamp(std::min(record.resolve_rect[3], int32_t(source->height / samples_y)) - record.resolve_rect[1], 0, dest_h - y), s);
  x = ScaleCoord(x, s);
  y = ScaleCoord(y, s);
  list->setFramebuffer(framebuffer);
  list->setGraphicsPipelineLayout(draw_layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list->setGraphicsDescriptorSet(draw_sets_[i].get(), i);
  list->setPipeline(pipeline.get());
  if (using_vulkan_) {
    const uint64_t address = ring_.buffer->getDeviceAddress() + uint64_t(offset);
    const uint64_t push[3] = {address, address, address};
    list->setGraphicsPushConstants(0, push, 0, uint32_t(sizeof(push)));
  } else {
    for (uint32_t i = 0; i < 3; ++i) list->setGraphicsRootDescriptor(ring_.buffer->at(uint64_t(offset)), i);
  }
  list->setViewports(plume::RenderViewport(float(x), float(y), float(w), float(h)));
  list->setScissors(plume::RenderRect(x, y, x + w, y + h));
  list->drawInstanced(3, 1, 0, 0);
  textures_.EndResolve(list, record.resolve_dest);
  return true;
}

bool GraphicsSystem::BlitFrontBuffer(plume::RenderCommandList* list, const TextureFetch& front_buffer, uint32_t width, uint32_t height) {
  const uint32_t texture = textures_.ResolvedIndex(front_buffer);
  if (!texture || !draw_ready_) return false;
  if (!blit_pipeline_) {
    if (using_vulkan_) {
      blit_vs_ = device_->createShader(shaders::g_blitVSSpirv, sizeof(shaders::g_blitVSSpirv), "VSMain", plume::RenderShaderFormat::SPIRV);
      blit_ps_ = device_->createShader(shaders::g_blitPSSpirv, sizeof(shaders::g_blitPSSpirv), "PSMain", plume::RenderShaderFormat::SPIRV);
    } else {
      blit_vs_ = device_->createShader(shaders::g_blitVSDxil, sizeof(shaders::g_blitVSDxil), "VSMain", plume::RenderShaderFormat::DXIL);
      blit_ps_ = device_->createShader(shaders::g_blitPSDxil, sizeof(shaders::g_blitPSDxil), "PSMain", plume::RenderShaderFormat::DXIL);
    }
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = draw_layout_.get();
    desc.vertexShader = blit_vs_.get();
    desc.pixelShader = blit_ps_.get();
    desc.renderTargetFormat[0] = kColorFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    blit_pipeline_ = device_->createGraphicsPipeline(desc);
    if (!blit_pipeline_) {
      REXGPU_ERROR("[gfx] blit pipeline creation failed");
      return false;
    }
  }
  BlitConstants constants = {texture, kBlitSampler, REXCVAR_GET(cherry_gfx_fxaa) ? 1u : 0u, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}};
  const int64_t offset = ring_.Alloc(sizeof(constants), kConstantAlign);
  if (offset < 0) return false;
  std::memcpy(ring_.mapped + offset, &constants, sizeof(constants));

  // Scaled evenly and centred, so a window of another shape gets bars rather than a stretch, unless stretching is asked for.
  const bool stretch = REXCVAR_GET(cherry_gfx_stretch);
  const float scale = std::min(float(width) / float(front_buffer.width()), float(height) / float(front_buffer.height()));
  const float w = stretch ? float(width) : float(front_buffer.width()) * scale;
  const float h = stretch ? float(height) : float(front_buffer.height()) * scale;
  list->setGraphicsPipelineLayout(draw_layout_.get());
  for (uint32_t i = 0; i < 4; ++i) list->setGraphicsDescriptorSet(draw_sets_[i].get(), i);
  list->setPipeline(blit_pipeline_.get());
  if (using_vulkan_) {
    const uint64_t address = ring_.buffer->getDeviceAddress() + uint64_t(offset);
    const uint64_t push[3] = {address, address, address};
    list->setGraphicsPushConstants(0, push, 0, uint32_t(sizeof(push)));
  } else {
    list->setGraphicsRootDescriptor(ring_.buffer->at(uint64_t(offset)), 0);
    list->setGraphicsRootDescriptor(ring_.buffer->at(uint64_t(offset)), 1);
    list->setGraphicsRootDescriptor(ring_.buffer->at(uint64_t(offset)), 2);
  }
  list->setViewports(plume::RenderViewport((float(width) - w) * 0.5f, (float(height) - h) * 0.5f, w, h));
  list->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  list->drawInstanced(3, 1, 0, 0);
  return true;
}

}  // namespace cherry::gpu
