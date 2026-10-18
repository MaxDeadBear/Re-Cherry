// Guest textures, described by their fetch constants, as bindless host textures and samplers.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace plume {
struct RenderDevice;
struct RenderDescriptorSet;
struct RenderCommandList;
struct RenderTexture;
struct RenderTextureView;
struct RenderSampler;
struct RenderFramebuffer;
}  // namespace plume

namespace cherry::gpu {

struct UploadRing;

// A GPUTEXTURE_FETCH_CONSTANT (XDK type in the IDB): six dwords, at device + 0x480 + 24 * slot.
struct TextureFetch {
  uint32_t words[6] = {};

  bool valid() const { return (words[0] & 3) == 2; }
  bool tiled() const { return (words[0] >> 31) & 1; }
  uint32_t pitch_texels() const { return ((words[0] >> 22) & 0x1FF) << 5; }
  uint32_t sign_x() const { return (words[0] >> 2) & 3; }
  uint32_t clamp(int axis) const { return (words[0] >> (10 + 3 * axis)) & 7; }
  uint32_t format() const { return words[1] & 0x3F; }
  uint32_t endian() const { return (words[1] >> 6) & 3; }
  // Physical. Texture objects hold a virtual base and SetTexture converts it into the device's fetch constants, so both forms are canonicalised the way D3D does: drop the view bits, and add the page the 0xE0000000 view is offset by.
  uint32_t address() const {
    const uint32_t raw = words[1] & 0xFFFFF000u;
    return (((raw >> 20) + 512) & 0x1000) + (raw & 0x1FFFFFFF);
  }
  // GPUTEXTURE_FETCH_CONSTANT.Size, decoded as the game's sub_829E4470 does: 1D 24 bits, 3D 11:11:10, otherwise 13:13, each plus one (three with a border); a Stacked 2D texture keeps its depth in bits 26..31.
  bool volume() const { return dimension() == 2; }
  bool stacked() const { return (words[1] >> 10) & 1; }
  uint32_t size_bias() const { return ((words[3] >> 30) & 2) + 1; }
  uint32_t width() const { return (dimension() == 0 ? words[2] & 0xFFFFFF : volume() ? words[2] & 0x7FF : words[2] & 0x1FFF) + size_bias(); }
  uint32_t height() const { return dimension() == 0 ? 1 : (volume() ? (words[2] >> 11) & 0x7FF : (words[2] >> 13) & 0x1FFF) + size_bias(); }
  uint32_t depth() const { return volume() ? (words[2] >> 22) + size_bias() : stacked() ? (words[2] >> 26) + 1 : 1; }
  uint32_t swizzle() const { return (words[3] >> 1) & 0xFFF; }
  uint32_t mag_filter() const { return (words[3] >> 19) & 3; }
  uint32_t min_filter() const { return (words[3] >> 21) & 3; }
  uint32_t mip_filter() const { return (words[3] >> 23) & 3; }
  uint32_t aniso() const { return (words[3] >> 25) & 7; }
  uint32_t border_color() const { return words[5] & 3; }
  uint32_t dimension() const { return (words[5] >> 9) & 3; }  // 0 1D, 1 2D, 2 3D, 3 cube
  bool packed_mips() const { return (words[5] >> 11) & 1; }
};

class TextureCache {
 public:
  void Initialize(plume::RenderDevice* device, plume::RenderDescriptorSet* textures_2d, plume::RenderDescriptorSet* textures_3d, plume::RenderDescriptorSet* textures_cube, plume::RenderDescriptorSet* samplers, uint32_t texture_capacity, uint32_t sampler_capacity);
  void Shutdown();

  // A new frame: textures are re-checked against guest memory once each per frame.
  // Releases what replaced resolve textures left behind once no frame in flight can still use it, and evicts textures no frame has used for a while.
  void BeginFrame(uint64_t frame_index);

  // The 2D heap index of the texture a fetch constant describes, untiling and uploading it on first use or when its guest bytes changed; 0 (the white placeholder) when it cannot be expressed.
  uint32_t Texture2D(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch);
  uint32_t Sampler(const TextureFetch& fetch);
  // The 3D heap index of a volume or stack a fetch describes: the frame's resolve into it, or its guest bytes (base level only).
  uint32_t Texture3D(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch);
  // The cube heap index of a cube texture, all six faces with their mips.
  uint32_t TextureCube(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch);

  // The host texture a resolve writes for the guest texture at the destination's address, (re)created at the source's format and the destination's size.
  // A copy gets it in COPY_DEST; a resolve drawn by a shader (`framebuffer` non-null, depth) gets it as a render target in COLOR_WRITE with its framebuffer.
  // Fetches of that address use it from then on instead of the guest bytes, which no GPU wrote.
  // A layered destination (UE3's colour-grading LUT, a 16x16 stack resolved a slice at a time and fetched with tfetch3D) is created 3D and registered in the 3D heap.
  // `scale` is the source target's render resolution scale, which the destination is created at.
  plume::RenderTexture* BeginResolve(plume::RenderCommandList* list, const TextureFetch& dest, uint32_t host_format, float scale, plume::RenderFramebuffer** framebuffer = nullptr);
  void EndResolve(plume::RenderCommandList* list, const TextureFetch& dest);
  // The heap index of the resolved texture a fetch describes (any resolve at its address if none matches its size), or 0.
  uint32_t ResolvedIndex(const TextureFetch& fetch) const;
  // The scale of the resolve Texture2D gives this fetch, 1 for guest bytes: its texture is that many times the guest size.
  float ResolvedScale(const TextureFetch& fetch) const {
    auto found = resolved_.find(ResolveKey{fetch.address(), fetch.width(), fetch.height(), 1});
    return found != resolved_.end() ? found->second.scale : 1.0f;
  }
  // A 2D heap index for a texture the renderer binds itself, such as a depth target a resolve reads; 0 when the heap is full.
  uint32_t ReserveIndex() { return AllocateIndex(); }
  // Returns a ReserveIndex index; the caller has made sure no frame in flight still reads it.
  void ReleaseIndex(uint32_t index) {
    if (index) free_indices_[uint32_t(Kind::k2D)].push_back(index);
  }

  // 0 keeps each sampler's own anisotropy; 2..16 forces that level on linearly filtered, mipmapped samplers.
  void SetForcedAnisotropy(uint32_t level) { forced_anisotropy_ = level; }

  // A guest size at a render resolution scale; guest sizes still key resolves and targets.
  static uint32_t Scaled(uint32_t size, float scale);
  // Forgets every resolved texture once the GPU is idle, for a change of render resolution.
  void DropResolved();

 private:
  struct Entry {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t heap_index = 0;
    uint32_t host_format = 0;  // plume::RenderFormat
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    float scale = 1.0f;  // resolves: the host size over the guest's
    uint64_t content_hash = 0;
    uint64_t checked_frame = ~0ull;
    // The physical bytes it was uploaded from, and the write-watch stamp they were hashed under (guest_writes.h).
    uint32_t watch_start = 0;
    uint32_t watch_length = 0;
    uint64_t watch_stamp = 0;
    bool rewritten = false;  // its pages were written with the same bytes, so it is hashed every frame instead
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;  // resolves drawn by a shader
  };

  enum class Kind : uint32_t { k2D, k3D, kCube };
  uint32_t UploadTexture(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch, Kind kind);
  // Copies one level of one layer, or for a 3D texture all its slices, from rows of blocks.
  bool UploadLevel(plume::RenderCommandList* list, UploadRing& ring, const TextureFetch& fetch, const Entry& entry, uint32_t level, uint32_t layer, uint32_t slices, const std::vector<uint8_t>& rows, uint32_t row_bytes, uint32_t rows_count);

  plume::RenderDevice* device_ = nullptr;
  plume::RenderDescriptorSet* textures_2d_ = nullptr;
  plume::RenderDescriptorSet* textures_3d_ = nullptr;
  plume::RenderDescriptorSet* textures_cube_ = nullptr;
  uint32_t next_volume_index_ = 1;
  uint32_t next_cube_index_ = 1;
  // Heap indices of evicted textures, free again once no frame in flight can still sample them; by Kind.
  std::vector<uint32_t> free_indices_[3];
  std::vector<std::tuple<uint64_t, Kind, uint32_t>> retired_indices_;
  uint32_t AllocateIndex(Kind kind);
  void EvictUnused();
  plume::RenderDescriptorSet* samplers_set_ = nullptr;
  uint32_t texture_capacity_ = 0;
  uint32_t sampler_capacity_ = 0;
  uint32_t next_texture_index_ = 1;
  uint32_t AllocateIndex() { return AllocateIndex(Kind::k2D); }
  uint32_t next_sampler_index_ = 1;
  uint64_t frame_index_ = 0;
  uint32_t forced_anisotropy_ = 0;
  // address, format, width, height, tiled, endian, swizzle, signed | levels | kind
  using TextureKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
  struct TextureKeyHash {
    size_t operator()(const TextureKey& key) const {
      const uint32_t words[8] = {std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), std::get<4>(key), std::get<5>(key), std::get<6>(key), std::get<7>(key)};
      uint64_t hash = 0xCBF29CE484222325ull;
      for (uint32_t word : words) hash = (hash ^ word) * 0x100000001B3ull;
      return size_t(hash ^ (hash >> 32));
    }
  };
  std::unordered_map<TextureKey, Entry, TextureKeyHash> textures_;
  std::unordered_map<uint64_t, std::pair<uint32_t, std::unique_ptr<plume::RenderSampler>>> samplers_;
  // address, width, height, depth: one guest address is resolved at several sizes in one frame.
  using ResolveKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
  static ResolveKey KeyOf(const TextureFetch& fetch) { return {fetch.address(), fetch.width(), fetch.height(), fetch.depth()}; }
  std::map<ResolveKey, Entry> resolved_;
  // Replaced resolve textures and the frame they were replaced in; the GPU may still be using them.
  std::vector<std::pair<uint64_t, Entry>> retired_;
  std::vector<uint32_t> unsupported_reported_;
};

}  // namespace cherry::gpu
