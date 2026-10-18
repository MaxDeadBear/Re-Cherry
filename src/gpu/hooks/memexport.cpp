#include <cstring>

#include "re_cherry_init.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/guest_memory.h"

// UE3's texture-pool defragmenter moves texture memory on the GPU by drawing points with memory export; with no guest GPU the move is done here instead.

namespace {

void XeTexturePool_GpuMemmove_hook(uint32_t dst, uint32_t src, uint32_t size) {
  if (!dst || !src || !size) return;
  uint8_t* base = cherry::gpu::GuestBase();
  std::memmove(REX_RAW_ADDR(dst), REX_RAW_ADDR(src), size);
  static uint32_t logged = 0;
  if (logged++ < 4) REXGPU_INFO("[gfx] texture pool move {:08X} -> {:08X}, {} bytes, done on the CPU", src, dst, size);
}

}  // namespace

// XeTexturePool_GpuMemmove(void* dst, const void* src, INT size)
REX_HOOK(XeTexturePool_GpuMemmove, XeTexturePool_GpuMemmove_hook)
