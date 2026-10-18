// A frame's slice of CPU-written GPU memory: textures staged before the render pass, then the draws' geometry and constant blocks.
#pragma once

#include <cstdint>

namespace plume {
struct RenderBuffer;
}  // namespace plume

namespace cherry::gpu {

struct UploadRing {
  plume::RenderBuffer* buffer = nullptr;
  uint8_t* mapped = nullptr;
  uint64_t capacity = 0;
  uint64_t used = 0;

  // The offset of `bytes` of space at `alignment`, or -1 when the frame's slice is full.
  int64_t Alloc(uint64_t bytes, uint64_t alignment) {
    const uint64_t offset = (used + alignment - 1) & ~(alignment - 1);
    if (offset + bytes > capacity) return -1;
    used = offset + bytes;
    return int64_t(offset);
  }
};

}  // namespace cherry::gpu
