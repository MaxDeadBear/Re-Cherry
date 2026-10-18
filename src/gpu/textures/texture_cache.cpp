#include "gpu/textures/texture_cache.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/debug/texture_dump.h"
#include "gpu/guest_memory.h"
#include "gpu/guest_writes.h"
#include "gpu/debug/frame_timing.h"
#include "gpu/textures/texture_codec.h"
#include "gpu/textures/texture_layout.h"
#include "gpu/textures/xenos_format.h"
#include "gpu/upload_ring.h"

namespace cherry::gpu {

namespace {

// Physical memory is mapped at the 0xA0000000 view with no host offset.
constexpr uint32_t kPhysicalView = 0xA0000000u;
constexpr uint32_t kPhysicalBytes = 0x20000000u;
constexpr uint32_t kCopyPlacementAlign = 512;
constexpr uint32_t kCopyRowAlign = 256;

plume::RenderSwizzle XenosSwizzle(uint32_t selector) {
  switch (selector) {
    case 0: return plume::RenderSwizzle::R;
    case 1: return plume::RenderSwizzle::G;
    case 2: return plume::RenderSwizzle::B;
    case 3: return plume::RenderSwizzle::A;
    case 4: return plume::RenderSwizzle::ZERO;
    default: return plume::RenderSwizzle::ONE;
  }
}

plume::RenderTextureAddressMode XenosClamp(uint32_t mode) {
  switch (mode) {
    case 0: return plume::RenderTextureAddressMode::WRAP;
    case 1: return plume::RenderTextureAddressMode::MIRROR;
    case 3: case 5: case 7: return plume::RenderTextureAddressMode::MIRROR_ONCE;
    case 6: return plume::RenderTextureAddressMode::BORDER;
    default: return plume::RenderTextureAddressMode::CLAMP;
  }
}

}  // namespace

void TextureCache::Initialize(plume::RenderDevice* device, plume::RenderDescriptorSet* textures_2d, plume::RenderDescriptorSet* textures_3d, plume::RenderDescriptorSet* textures_cube, plume::RenderDescriptorSet* samplers, uint32_t texture_capacity, uint32_t sampler_capacity) {
  device_ = device;
  textures_2d_ = textures_2d;
  textures_3d_ = textures_3d;
  textures_cube_ = textures_cube;
  samplers_set_ = samplers;
  texture_capacity_ = texture_capacity;
  sampler_capacity_ = sampler_capacity;
}

void TextureCache::Shutdown() {
  resolved_.clear();
  retired_.clear();
  retired_indices_.clear();
  for (auto& free : free_indices_) free.clear();
  textures_.clear();
  samplers_.clear();
  device_ = nullptr;
}

uint32_t TextureCache::Sampler(const TextureFetch& fetch) {
  // dword4: MinMipLevel 2..5, MaxMipLevel 6..9, LODBias 12..21 (signed, in 1/32 steps, as SetSamplerState_MipMapLodBias writes it).
  const uint32_t lod = (fetch.words[4] >> 2) & 0xFF;
  const int32_t lod_bias = int32_t(fetch.words[4] << 10) >> 22;
  // The forced level applies to minified, mipmapped sampling only; point-sampled and unmipped lookups (shadow maps, the HUD) keep the game's.
  const bool mipmapped = fetch.min_filter() != 0 && fetch.mip_filter() <= 1 && (lod >> 4) > (lod & 0xF);
  const uint32_t forced = mipmapped && forced_anisotropy_ >= 2 ? std::min(forced_anisotropy_, 16u) : 0;
  const uint64_t key = uint64_t(fetch.clamp(0) | (fetch.clamp(1) << 3) | (fetch.clamp(2) << 6) | (fetch.mag_filter() << 9) | (fetch.min_filter() << 11) | (fetch.mip_filter() << 13) | (fetch.aniso() << 15) | (fetch.border_color() << 18)) | (uint64_t(lod) << 20) | (uint64_t(uint32_t(lod_bias) & 0x3FF) << 28) | (uint64_t(forced) << 38);
  auto found = samplers_.find(key);
  if (found != samplers_.end()) return found->second.first;
  if (next_sampler_index_ >= sampler_capacity_) return 0;
  plume::RenderSamplerDesc desc;
  desc.magFilter = fetch.mag_filter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  desc.minFilter = fetch.min_filter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  desc.mipmapMode = fetch.mip_filter() == 1 ? plume::RenderMipmapMode::LINEAR : plume::RenderMipmapMode::NEAREST;
  desc.addressU = XenosClamp(fetch.clamp(0));
  desc.addressV = XenosClamp(fetch.clamp(1));
  desc.addressW = XenosClamp(fetch.clamp(2));
  // Aniso 0 is off; 1..5 are 1:1 to 16:1.
  if (forced) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = forced;
  } else if (fetch.aniso() >= 2 && fetch.aniso() <= 5) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (fetch.aniso() - 1);
  }
  desc.borderColor = fetch.border_color() == 1 ? plume::RenderBorderColor::OPAQUE_WHITE : plume::RenderBorderColor::TRANSPARENT_BLACK;
  desc.minLOD = float(lod & 0xF);
  desc.maxLOD = float(std::max(lod & 0xF, lod >> 4));
  desc.mipLODBias = float(lod_bias) / 32.0f;
  auto sampler = device_->createSampler(desc);
  if (!sampler) return 0;
  const uint32_t index = next_sampler_index_++;
  samplers_set_->setSampler(index, sampler.get());
  samplers_.emplace(key, std::pair{index, std::move(sampler)});
  return index;
}

uint32_t TextureCache::Texture2D(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch) {
  if (!fetch.valid()) return 0;
  // Something the frame renders: no GPU wrote the guest bytes, so the resolve's host texture stands in for them.
  if (auto resolved = resolved_.find(ResolveKey{fetch.address(), fetch.width(), fetch.height(), 1}); resolved != resolved_.end()) return resolved->second.heap_index;
  const uint32_t format = fetch.format();
  const HostTextureFormat host = MapTextureFormat(format);
  if (host.format == plume::RenderFormat::UNKNOWN) {
    static uint32_t reported = 0;
    if (reported++ < 40) {
      auto resolved = resolved_.lower_bound(ResolveKey{fetch.address(), 0, 0, 0});
      const bool there = resolved != resolved_.end() && std::get<0>(resolved->first) == fetch.address();
      REXGPU_WARN("[gfx] untranslated texture format {} {}x{} at {:08X}{}; drawn with the placeholder", format, fetch.width(), fetch.height(), fetch.address(), !there ? ", nothing resolved there" : std::format(", a {}x{} resolve is there", resolved->second.width, resolved->second.height));
    }
    return 0;
  }
  return UploadTexture(list, ring, fetch, Kind::k2D);
}

uint32_t TextureCache::Texture3D(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch) {
  if (!fetch.valid()) return 0;
  auto found = resolved_.find(KeyOf(fetch));
  if (found != resolved_.end() && found->second.depth > 1) return found->second.heap_index;
  if (MapTextureFormat(fetch.format()).format == plume::RenderFormat::UNKNOWN) return 0;
  return UploadTexture(list, ring, fetch, Kind::k3D);
}

uint32_t TextureCache::TextureCube(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch) {
  if (!fetch.valid() || fetch.dimension() != 3) return 0;
  if (MapTextureFormat(fetch.format()).format == plume::RenderFormat::UNKNOWN) return 0;
  return UploadTexture(list, ring, fetch, Kind::kCube);
}

uint32_t TextureCache::UploadTexture(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch, Kind kind) {
  const uint32_t format = fetch.format();
  const HostTextureFormat host = MapTextureFormat(format);
  const uint32_t width = fetch.width();
  const uint32_t height = fetch.height();
  const uint32_t address = fetch.address();
  const bool is_signed = fetch.sign_x() == 1;
  const bool volume = kind == Kind::k3D && fetch.volume();
  const uint32_t layers = kind == Kind::kCube ? 6 : kind == Kind::k3D ? fetch.depth() : 1;
  // MaxMipLevel (dword4 bits 6..9), as SetTexture clamped it; a level smaller than one texel does not exist. Volumes and stacks take their base level only.
  const uint32_t levels = kind == Kind::k3D ? 1 : std::min(((fetch.words[4] >> 6) & 0xF) + 1, 32 - uint32_t(std::countl_zero(std::max(width, height))));
  // Gamma colour is decoded by the host: an sRGB format for block-compressed textures, linear 16-bit texels for RGBA8 and the formats expanded to it.
  const uint32_t gamma = GammaComponents(fetch.words);
  const bool linearize = gamma && host.format == plume::RenderFormat::R8G8B8A8_UNORM;
  auto key = std::tuple{address, format, width, height, uint32_t(fetch.tiled()), fetch.endian(), fetch.swizzle(), uint32_t(is_signed) | (levels << 1) | (uint32_t(kind) << 6) | (gamma << 8)};
  if (auto found = textures_.find(key); found != textures_.end() && found->second.texture && found->second.checked_frame != ~0ull) {
    Entry& cached = found->second;
    if (cached.checked_frame == frame_index_) return cached.heap_index;
    // Nothing wrote its pages since they were last hashed: the upload stands without reading them again.
    if (!WrittenSince(cached.watch_start, cached.watch_length, cached.watch_stamp)) {
      cached.checked_frame = frame_index_;
      return cached.heap_index;
    }
  }

  // Each level's guest bytes, where D3DBaseTexture_GetMipLayout puts them: tiled levels pad to 32x32-block macro tiles, and the smallest share the packed tail's tile. Faces and stack slices are separate surfaces; a volume's slices tile together.
  struct Level {
    MipLocation location;
    uint32_t blocks_x, blocks_y, pitch_blocks, rows_blocks, span;
    const uint8_t* source;
  };
  uint8_t* base = GuestBase();
  const uint32_t surfaces = volume ? 1 : layers;
  std::vector<Level> guest;
  guest.reserve(size_t(levels) * surfaces);
  uint32_t level_count = levels;
  uint32_t range_start = ~0u, range_end = 0;
  for (uint32_t l = 0; l < level_count; ++l) {
    for (uint32_t layer = 0; layer < surfaces; ++layer) {
      Level level;
      level.location = LocateMip(fetch.words, l, layer);
      const uint32_t level_address = GuestPhysicalAddress(level.location.address);
      level.blocks_x = (level.location.width + host.block - 1) / host.block;
      level.blocks_y = (level.location.height + host.block - 1) / host.block;
      level.pitch_blocks = std::max(1u, level.location.row_pitch * host.block / host.block_bytes);
      level.rows_blocks = std::max(1u, (level.location.rows + host.block - 1) / host.block);
      level.span = level.pitch_blocks * level.rows_blocks * host.block_bytes * (volume ? (layers + 3) & ~3u : 1);
      if (level_address == 0 || uint64_t(level_address) + level.span > kPhysicalBytes) {
        if (l == 0) return 0;
        level_count = l;
        break;
      }
      level.source = base + kPhysicalView + level_address;
      range_start = std::min(range_start, level_address);
      range_end = std::max(range_end, level_address + level.span);
      guest.push_back(level);
    }
  }
  guest.resize(size_t(level_count) * surfaces);
  // Armed before the bytes are read, so a write landing while they are hashed is caught next frame; bytes the guest rewrites unchanged are hashed every frame instead, as a watch would fault every frame.
  auto existing = textures_.find(key);
  const bool rewritten = existing != textures_.end() && existing->second.rewritten;
  const uint64_t watch_stamp = rewritten ? 0 : ArmWriteWatch(range_start, range_end - range_start);
  uint64_t hash = 0;
  for (const Level& level : guest) hash = XXH3_64bits_withSeed(level.source, level.span, hash);

  Entry& entry = textures_[key];
  entry.checked_frame = frame_index_;
  if (entry.texture && hash == entry.content_hash && entry.watch_stamp) entry.rewritten = true;
  entry.watch_start = range_start;
  entry.watch_length = range_end - range_start;
  entry.watch_stamp = entry.rewritten ? 0 : watch_stamp;
  if (entry.texture && hash == entry.content_hash) return entry.heap_index;

  static uint32_t large_logged = 0;
  if ((width >= 1024 || height >= 1024) && large_logged++ < 24) REXGPU_INFO("[gfx] uploading {}x{} format {} at {:08X} ({}, {} levels){}", width, height, format, address, fetch.tiled() ? "tiled" : "linear", level_count, entry.texture ? ", changed since last frame" : "");
  const uint32_t log2_bpp = Log2(host.block_bytes);
  const uint32_t slices = volume ? layers : 1;
  std::vector<std::vector<uint8_t>> level_rows(guest.size());
  for (size_t g = 0; g < guest.size(); ++g) {
    const Level& level = guest[g];
    const uint32_t row_bytes = level.blocks_x * host.block_bytes;
    std::vector<uint8_t>& rows = level_rows[g];
    rows.resize(size_t(row_bytes) * level.blocks_y * slices);
    for (uint32_t z = 0; z < slices; ++z) {
      for (uint32_t y = 0; y < level.blocks_y; ++y) {
        uint8_t* dst = rows.data() + (size_t(z) * level.blocks_y + y) * row_bytes;
        for (uint32_t x = 0; x < level.blocks_x; ++x) {
          const uint32_t gx = x + level.location.tail_x, gy = y + level.location.tail_y;
          uint64_t offset;
          if (volume) offset = fetch.tiled() ? uint64_t(TiledOffset3D(gx, gy, z, level.pitch_blocks, level.rows_blocks, log2_bpp)) * host.block_bytes : ((uint64_t(z) * level.rows_blocks + gy) * level.pitch_blocks + gx) * host.block_bytes;
          else offset = fetch.tiled() ? uint64_t(TiledOffset2D(gx, gy, level.pitch_blocks, log2_bpp)) * host.block_bytes : (uint64_t(gy) * level.pitch_blocks + gx) * host.block_bytes;
          if (offset + host.block_bytes <= level.span) std::memcpy(dst + size_t(x) * host.block_bytes, level.source + offset, host.block_bytes);
        }
      }
    }
    ApplyEndian(rows.data(), rows.size(), fetch.endian());
    if (g == 0 && kind == Kind::k2D && host.block > 1) debug::DumpTexture(address, format, width, height, rows.data(), row_bytes);
    if (host.expand_to_rgba8) {
      const size_t texels = size_t(level.location.width) * level.location.height * slices;
      std::vector<uint8_t> expanded(texels * 4);
      ExpandPackedToRgba8(rows.data(), expanded.data(), uint32_t(texels), format, host.block_bytes);
      rows.swap(expanded);
    }
    if (linearize) rows = LinearizeGammaRgba8(rows, gamma);
  }

  if (!entry.texture) {
    uint32_t index = 0;
    index = AllocateIndex(kind);
    if (!index) {
      textures_.erase(key);
      return 0;
    }
    plume::RenderFormat host_format = host.format;
    if (is_signed && SnormTwin(host_format) != plume::RenderFormat::UNKNOWN) host_format = SnormTwin(host_format);
    if ((gamma & 7) == 7 && SrgbTwin(host_format) != plume::RenderFormat::UNKNOWN) host_format = SrgbTwin(host_format);
    if (linearize) host_format = plume::RenderFormat::R16G16B16A16_UNORM;
    // Block-compressed textures need block-aligned dimensions on the host.
    const uint32_t texture_width = host.block > 1 ? guest[0].blocks_x * host.block : width;
    const uint32_t texture_height = host.block > 1 ? guest[0].blocks_y * host.block : height;
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(texture_width, texture_height, level_count, host_format);
    plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(host_format);
    if (kind == Kind::k3D) {
      desc = plume::RenderTextureDesc::Texture3D(texture_width, texture_height, layers, 1, host_format);
      view = plume::RenderTextureViewDesc::Texture3D(host_format);
    } else if (kind == Kind::kCube) {
      desc = plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, texture_width, texture_height, 1, level_count, 6, host_format, plume::RenderTextureFlag::CUBE);
      view = plume::RenderTextureViewDesc::TextureCube(host_format);
    }
    entry.texture = device_->createTexture(desc);
    if (!entry.texture) {
      textures_.erase(key);
      return 0;
    }
    const uint32_t swizzle = fetch.swizzle();
    view.componentMapping = plume::RenderComponentMapping(XenosSwizzle(swizzle & 7), XenosSwizzle((swizzle >> 3) & 7), XenosSwizzle((swizzle >> 6) & 7), XenosSwizzle((swizzle >> 9) & 7));
    entry.view = entry.texture->createTextureView(view);
    entry.host_format = uint32_t(host_format);
    entry.width = texture_width;
    entry.height = texture_height;
    entry.depth = kind == Kind::k3D ? layers : 1;
    entry.levels = level_count;
    entry.heap_index = index;
    plume::RenderDescriptorSet* heap = kind == Kind::k2D ? textures_2d_ : kind == Kind::k3D ? textures_3d_ : textures_cube_;
    heap->setTexture(entry.heap_index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ, entry.view.get());
  }
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  bool uploaded = true;
  for (size_t g = 0; g < guest.size() && uploaded; ++g) {
    const uint32_t level = uint32_t(g / surfaces), layer = kind == Kind::kCube ? uint32_t(g % surfaces) : 0;
    const uint32_t row_bytes = linearize ? (host.expand_to_rgba8 ? guest[g].location.width : guest[g].blocks_x) * 8 : host.expand_to_rgba8 ? guest[g].location.width * 4 : guest[g].blocks_x * host.block_bytes;
    const uint32_t rows_count = host.expand_to_rgba8 ? guest[g].location.height : guest[g].blocks_y;
    if (kind == Kind::k3D && !volume) {
      // A stack's slices are separate guest surfaces but one host 3D texture: gather them before the copy.
      if (g != 0) continue;
      std::vector<uint8_t> all;
      for (const auto& slice : level_rows) all.insert(all.end(), slice.begin(), slice.end());
      uploaded = UploadLevel(list, ring, fetch, entry, 0, 0, layers, all, row_bytes, rows_count);
      continue;
    }
    uploaded = UploadLevel(list, ring, fetch, entry, level, layer, slices, level_rows[g], row_bytes, rows_count);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::SHADER_READ));
  // Out of upload space this frame: the next frame tries again.
  if (!uploaded) {
    entry.checked_frame = ~0ull;
    return entry.heap_index;
  }
  entry.content_hash = hash;
  return entry.heap_index;
}

uint32_t TextureCache::Scaled(uint32_t size, float scale) {
  return std::max(1u, uint32_t(std::lround(double(size) * scale)));
}

void TextureCache::DropResolved() {
  for (auto& [key, entry] : resolved_) {
    if (entry.heap_index) free_indices_[uint32_t(std::get<3>(key) > 1 ? Kind::k3D : Kind::k2D)].push_back(entry.heap_index);
  }
  resolved_.clear();
  retired_.clear();
}

plume::RenderTexture* TextureCache::BeginResolve(plume::RenderCommandList* list, const TextureFetch& dest, uint32_t host_format, float scale, plume::RenderFramebuffer** framebuffer) {
  const uint32_t width = Scaled(dest.width(), scale);
  const uint32_t height = Scaled(dest.height(), scale);
  const uint32_t depth = dest.depth();
  const bool render_target = framebuffer != nullptr && depth == 1;
  if (framebuffer && !render_target) return nullptr;
  Entry& entry = resolved_[KeyOf(dest)];
  if (!entry.texture || entry.host_format != host_format || entry.width != width || entry.height != height) {
    // The previous texture may still be in use by a frame in flight; it is released a few frames later.
    if (entry.texture) {
      Entry& old = retired_.emplace_back(frame_index_, Entry{}).second;
      old.texture = std::move(entry.texture);
      old.view = std::move(entry.view);
      old.framebuffer = std::move(entry.framebuffer);
    }
    const plume::RenderFormat format = plume::RenderFormat(host_format);
    // A 2D destination is a render target either way: UE3 resolves one address both by copy and through a shader within a frame, and a texture of each kind would be recreated every time it switched.
    entry.texture = device_->createTexture(depth > 1 ? plume::RenderTextureDesc::Texture3D(width, height, depth, 1, format) : plume::RenderTextureDesc::ColorTarget(width, height, format));
    if (!entry.texture) {
      resolved_.erase(KeyOf(dest));
      return nullptr;
    }
    entry.view = entry.texture->createTextureView(depth > 1 ? plume::RenderTextureViewDesc::Texture3D(format) : plume::RenderTextureViewDesc::Texture2D(format));
    entry.host_format = host_format;
    entry.width = width;
    entry.height = height;
    entry.depth = depth;
    entry.scale = scale;
    if (!entry.heap_index) entry.heap_index = AllocateIndex(depth > 1 ? Kind::k3D : Kind::k2D);
    if (entry.heap_index) (depth > 1 ? textures_3d_ : textures_2d_)->setTexture(entry.heap_index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ, entry.view.get());
  }
  if (render_target && !entry.framebuffer) {
    const plume::RenderTexture* attachment = entry.texture.get();
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &attachment;
    desc.colorAttachmentsCount = 1;
    entry.framebuffer = device_->createFramebuffer(desc);
  }
  if (render_target) {
    list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::COLOR_WRITE));
    *framebuffer = entry.framebuffer.get();
    return entry.framebuffer ? entry.texture.get() : nullptr;
  }
  list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::COPY_DEST));
  return entry.texture.get();
}

void TextureCache::EndResolve(plume::RenderCommandList* list, const TextureFetch& dest) {
  auto found = resolved_.find(KeyOf(dest));
  if (found != resolved_.end() && found->second.texture) list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(found->second.texture.get(), plume::RenderTextureLayout::SHADER_READ));
}

uint32_t TextureCache::AllocateIndex(Kind kind) {
  std::vector<uint32_t>& free = free_indices_[uint32_t(kind)];
  if (!free.empty()) {
    const uint32_t index = free.back();
    free.pop_back();
    return index;
  }
  uint32_t& next = kind == Kind::k2D ? next_texture_index_ : kind == Kind::k3D ? next_volume_index_ : next_cube_index_;
  return next < texture_capacity_ ? next++ : 0;
}

// UE3 streams textures in and out per level; one no frame has fetched for about a minute is dropped and uploaded again if it comes back.
void TextureCache::EvictUnused() {
  constexpr uint64_t kUnusedFrames = 1800;
  for (auto it = textures_.begin(); it != textures_.end();) {
    Entry& entry = it->second;
    if (!entry.texture || entry.checked_frame == ~0ull || entry.checked_frame + kUnusedFrames >= frame_index_) {
      ++it;
      continue;
    }
    retired_indices_.emplace_back(frame_index_, Kind((std::get<7>(it->first) >> 6) & 3), entry.heap_index);
    retired_.emplace_back(frame_index_, std::move(entry));
    it = textures_.erase(it);
  }
}

void TextureCache::BeginFrame(uint64_t frame_index) {
  frame_index_ = frame_index;
  InitGuestWriteWatch();
  if ((frame_index % 300) == 0) EvictUnused();
  // Two frames in flight, and one more for the present that may still read a front buffer.
  std::erase_if(retired_, [&](const auto& retired) { return retired.first + 3 <= frame_index; });
  std::erase_if(retired_indices_, [&](const auto& retired) {
    if (std::get<0>(retired) + 3 > frame_index) return false;
    if (std::get<2>(retired)) free_indices_[uint32_t(std::get<1>(retired))].push_back(std::get<2>(retired));
    return true;
  });
}

uint32_t TextureCache::ResolvedIndex(const TextureFetch& fetch) const {
  if (auto found = resolved_.find(KeyOf(fetch)); found != resolved_.end()) return found->second.heap_index;
  auto any = resolved_.lower_bound(ResolveKey{fetch.address(), 0, 0, 0});
  return any != resolved_.end() && std::get<0>(any->first) == fetch.address() ? any->second.heap_index : 0;
}

bool TextureCache::UploadLevel(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch, const Entry& entry, uint32_t level, uint32_t layer, uint32_t slices, const std::vector<uint8_t>& rows, uint32_t row_bytes, uint32_t rows_count) {
  debug::AddFrameCount(debug::FrameCount::TextureBytes, uint64_t(row_bytes) * rows_count * slices);
  const uint32_t pitch = (row_bytes + kCopyRowAlign - 1) & ~(kCopyRowAlign - 1);
  const int64_t offset = ring.Alloc(uint64_t(pitch) * rows_count * slices, kCopyPlacementAlign);
  if (offset < 0) return false;
  for (uint32_t y = 0; y < rows_count * slices; ++y) std::memcpy(ring.mapped + offset + uint64_t(y) * pitch, rows.data() + size_t(y) * row_bytes, row_bytes);
  const HostTextureFormat host = MapTextureFormat(fetch.format());
  // The footprint's row width is in texels: for block formats, the texels the padded row of blocks covers. Its size is the host level's, rounded up to whole blocks.
  const bool linear16 = plume::RenderFormat(entry.host_format) == plume::RenderFormat::R16G16B16A16_UNORM;
  const uint32_t row_texels = linear16 ? pitch / 8 : host.expand_to_rgba8 ? pitch / 4 : (pitch / host.block_bytes) * host.block;
  const uint32_t block = host.expand_to_rgba8 || linear16 ? 1 : host.block;
  const uint32_t level_width = (std::max(1u, entry.width >> level) + block - 1) / block * block;
  const uint32_t level_height = (std::max(1u, entry.height >> level) + block - 1) / block * block;
  auto source = plume::RenderTextureCopyLocation::PlacedFootprint(ring.buffer, plume::RenderFormat(entry.host_format), level_width, level_height, slices, row_texels, uint64_t(offset));
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(entry.texture.get(), level, layer), source);
  return true;
}

}  // namespace cherry::gpu
