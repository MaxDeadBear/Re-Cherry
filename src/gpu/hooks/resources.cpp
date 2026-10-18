#include <atomic>
#include <bit>
#include <cstring>

#include "gpu/hooks/hooks_internal.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include "core/guest_call.h"
#include "gpu/guest_memory.h"

// The D3D resource, lock, query and predication functions UE3 calls, written natively. Destroying a resource and allocating an occlusion slot still run the game's own code, through scratch stacks.

REX_IMPORT(__imp__sub_82BF5230, GuestXMemAlloc, u32(u32, u32));
REX_IMPORT(__imp__sub_82BF5368, GuestXMemFree, u32(u32, u32));
REX_IMPORT(__imp__D3DResource_Destroy, GuestDestroyResource, u32(u32));
REX_IMPORT(__imp__sub_829EEB70, GuestAllocOcclusionSlot, u32(u32, u32, u32));

namespace {

using namespace cherry::gpu;
using namespace cherry::gpu::hooks;
using cherry::core::CallGuest;

constexpr uint32_t kCpuFence = 0x2B1C;
constexpr uint32_t kQueryBlocks = 0x55D0;

uint32_t AtomicAddGuest(uint32_t address, uint32_t delta) {
  auto& word = *reinterpret_cast<std::atomic<uint32_t>*>(GuestBase() + address);
  uint32_t stored = word.load();
  for (;;) {
    const uint32_t next = std::byteswap(stored) + delta;
    if (word.compare_exchange_weak(stored, std::byteswap(next))) return next;
  }
}

bool IsChildSurface(uint32_t common) {
  return (common & 0xF) == 4 && (common & 0x40000000);
}

uint32_t D3DResource_AddRef_hook(uint32_t resource) {
  const uint32_t count = AtomicAddGuest(resource + 4, 1);
  if (count == 1 && IsChildSurface(LoadGuestU32(resource))) D3DResource_AddRef_hook(LoadGuestU32(resource + 0x18));
  return count;
}

uint32_t D3DResource_Release_hook(uint32_t resource) {
  const uint32_t count = AtomicAddGuest(resource + 4, 0xFFFFFFFFu);
  if (count) return count;
  if (IsChildSurface(LoadGuestU32(resource))) D3DResource_Release_hook(LoadGuestU32(resource + 0x18));
  CallGuest(GuestDestroyResource, resource);
  return 0;
}

void D3DResource_GetDevice_hook(uint32_t resource, uint32_t device_out) {
  (void)resource;
  const uint32_t device = GuestDevice();
  if (device) AtomicAddGuest(device + 0x3C, 1);
  StoreGuestU32(device_out, device);
}

uint32_t D3DResource_GetType_hook(uint32_t resource) {
  const uint32_t common = LoadGuestU32(resource), type = common & 0xF;
  if (type == 3) {
    const uint32_t dimension = (LoadGuestU32(resource + 0x30) >> 9) & 3;
    if (dimension == 3) return 0x12;
    if (dimension == 2) return 0x11;
    if (dimension == 0) return 0x14;
    return (LoadGuestU32(resource + 0x20) & 0x400) ? 0x13 : 3;
  }
  if (IsChildSurface(common) && (LoadGuestU32(LoadGuestU32(resource + 0x18) + 0x30) & 0x600) == 0x400) return 0x10;
  return type;
}

uint32_t D3DResource_IsSet_hook(uint32_t resource, uint32_t device) {
  auto any = [&](uint32_t at, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
      if (LoadGuestU32(at + 4 * i) == resource) return 1u;
    return 0u;
  };
  switch (LoadGuestU32(resource) & 0xF) {
    case 1: return any(device + 0x3224, 16);
    case 2: return LoadGuestU32(device + 0x320C) == resource;
    case 3: return any(device + 0x3278, 26);
    case 4: return LoadGuestU32(device + 0x3210) == resource || LoadGuestU32(device + 0x3220) == resource;
    case 5: return LoadGuestU32(device + 0x2FD0) == resource;
    case 6: return LoadGuestU32(device + 0x3310) == resource;
    case 7: return LoadGuestU32(device + 0x330C) == resource;
    default: return 0;
  }
}

// Every fence is already passed, so the lock only counts itself and widens the CPU flush range the unlock clears.
uint32_t D3D_LockResource_hook(uint32_t resource, uint32_t reason, uint32_t is_mip, uint32_t base_address, uint32_t mip_address, uint32_t lock_address, uint32_t lock_size, uint32_t flags) {
  (void)reason;
  const uint32_t device = GuestDevice();
  PublishFences(device);
  AtomicAddGuest(resource, 0x100);
  if (!(flags & 0x12)) StoreGuestU32(device + 0x2FE0, 1);
  if (((flags & 0x10) || (LoadGuestU32(resource) & 0x200000)) && !(flags & 1)) {
    const bool mip = is_mip && mip_address;
    const uint32_t line_base = (mip ? mip_address : base_address) & ~0x7Fu, range_at = resource + (mip ? 0x18 : 0x14);
    const uint32_t offset = lock_address - line_base, first = offset >> 7, last = (offset + lock_size + 0x7F) >> 7;
    const uint32_t range = LoadGuestU32(range_at);
    uint32_t start = range >> 16, end = range & 0xFFFF;
    if (start & 0x8000) start = (start & 0x7FFF) << 4;
    if (end & 0x8000) end = (end & 0x7FFF) << 4;
    if (last > end) end = last;
    if (first < start) start = first;
    if (start >= 0x7FFF) start = (start + 0x80000) >> 4;
    if (end >= 0x7FFF) end = (end + 0x8000F) >> 4;
    StoreGuestU32(range_at, (start << 16) | end);
  }
  return (flags & 0x10) ? GuestPhysicalAddress(lock_address) + 0xC0000000u : lock_address;
}

void D3D_UnlockResource_hook(uint32_t resource, uint32_t base_address, uint32_t mip_address) {
  (void)base_address;
  if ((LoadGuestU32(resource) & 0xF00) == 0x100) {
    if (LoadGuestU32(resource + 0x14) != 0xFFFF0000u) StoreGuestU32(resource + 0x14, 0xFFFF0000u);
    if (mip_address && LoadGuestU32(resource + 0x18) != 0xFFFF0000u) StoreGuestU32(resource + 0x18, 0xFFFF0000u);
  }
  AtomicAddGuest(resource, 0xFFFFFF00u);
}

// A dcbf over the range; the host's caches are coherent.
uint32_t D3D_FlushCachedRange_hook(uint32_t start, uint32_t end) {
  (void)end;
  return start;
}

void D3D_InvalidateGpuCacheRange_hook(uint32_t address, uint32_t size) {
  (void)address;
  (void)size;
  StoreGuestU32(GuestDevice() + 0x2FE0, 1);
}

// The device outlives the game; its teardown only stops the GPU.
uint32_t D3DDevice_Release_hook(uint32_t device) {
  const uint32_t count = AtomicAddGuest(device + 0x3C, 0xFFFFFFFFu);
  if (!count) {
    uint8_t* base = GuestBase();
    REX_STORE_U8(device + 0x2B40, uint8_t(REX_LOAD_U8(device + 0x2B40) | 0x10));
  }
  return count;
}

void D3DDevice_GetDeviceCaps_hook(uint32_t device, uint32_t caps) {
  (void)device;
  uint8_t* base = GuestBase();
  std::memcpy(base + caps, base + 0x821A5868, 0x130);
}

void FreeEmptyQueryBlock(uint32_t device, uint32_t block) {
  if (LoadGuestU32(block) || LoadGuestU32(block + 4) || block == device + kQueryBlocks) return;
  for (uint32_t current = device + kQueryBlocks, next; (next = LoadGuestU32(current + 0xC));) {
    if (next != block) {
      current = next;
      continue;
    }
    CallGuest(GuestXMemFree, LoadGuestU32(block + 8), 0xB1800000u);
    StoreGuestU32(current + 0xC, LoadGuestU32(block + 0xC));
    CallGuest(GuestXMemFree, block, 0x24800000u);
    return;
  }
}

void ClearQuerySlot(uint32_t block, uint32_t slot) {
  uint8_t* base = GuestBase();
  const uint32_t index = uint32_t((int32_t(slot - LoadGuestU32(block + 8)) >> 2) / 16);
  REX_STORE_U8(block + (index >> 3), uint8_t(REX_LOAD_U8(block + (index >> 3)) & ~(1u << (index & 7))));
}

uint32_t D3DQuery_Release_hook(uint32_t query) {
  uint8_t* base = GuestBase();
  const uint32_t device = LoadGuestU32(query);
  const uint32_t count = AtomicAddGuest(query + 0xC, 0xFFFFFFFFu);
  if (count) return count;
  const uint32_t type = LoadGuestU32(query + 4);
  if (type == 9) {
    for (uint32_t i = 0; i < LoadGuestU32(query + 0x98); ++i) {
      const uint32_t block = LoadGuestU32(query + 0x58 + 4 * i);
      ClearQuerySlot(block, LoadGuestU32(query + 0x1C + 4 * i));
      FreeEmptyQueryBlock(device, block);
    }
  } else if (type == 10) {
    const uint32_t block = LoadGuestU32(query + 0x58);
    if (LoadGuestU32(query + 0x1C)) ClearQuerySlot(block, LoadGuestU32(query + 0x1C));
    FreeEmptyQueryBlock(device, block);
    if (REX_LOAD_U8(query + 0x10) == 2) REX_STORE_U8(device + 0x2B3F, uint8_t(REX_LOAD_U8(device + 0x2B3F) & 0x7F));
  }
  StoreGuestU32(query + 8, 0x78787878);
  CallGuest(GuestXMemFree, query, 0x24800000u);
  return 0;
}

uint32_t D3DDevice_CreateOcclusionQuery_hook(uint32_t device) {
  const uint32_t query = CallGuest(GuestXMemAlloc, 0x9Cu, 0x64800000u);
  if (!query) return 0;
  StoreGuestU32(query, device);
  StoreGuestU32(query + 0xC, 1);
  StoreGuestU32(query + 4, 9);
  StoreGuestU32(query + 0x98, 1);
  StoreGuestU32(query + 0x94, 1);
  for (uint32_t i = 0; i < LoadGuestU32(query + 0x98); ++i) {
    if (int32_t(CallGuest(GuestAllocOcclusionSlot, device, query + 0x58 + 4 * i, query + 0x1C + 4 * i)) < 0) {
      StoreGuestU32(query + 0x98, i);
      D3DQuery_Release_hook(query);
      return 0;
    }
  }
  uint8_t* base = GuestBase();
  REX_STORE_U8(query + 0x10, 1);
  return query;
}

void ResetScreenExtentState(uint32_t device) {
  uint8_t* base = GuestBase();
  REX_STORE_U8(device + 0x2B40, uint8_t(REX_LOAD_U8(device + 0x2B40) & 0x3F));
  REX_STORE_U8(device + 0x2B3F, uint8_t(REX_LOAD_U8(device + 0x2B3F) & 0xFC));
  StoreGuestU32(device + 0x20, LoadGuestU32(device + 0x20) & 0x3FFFFFFF);
}

// BEGIN (flags bit 1) and END (bit 0); the counters an occlusion END would have the GPU write start at the -275 sentinel, and D3DQuery_GetData answers natively.
void D3DQuery_Issue_hook(uint32_t query, uint32_t flags) {
  uint8_t* base = GuestBase();
  const uint32_t device = LoadGuestU32(query), type = LoadGuestU32(query + 4);
  if (flags & 2) {
    StoreGuestU32(query + 0x94, (REX_LOAD_U8(device + 0x2B3C) & 0x20) ? LoadGuestU32(device + 0x3344) : 1);
    if (type == 9) {
      OrGuestU64(device + 0x10, 0x0300000000000008ull);
    } else if (type == 10) {
      if (REX_LOAD_U8(device + 0x2B40) & 0x80) ResetScreenExtentState(device);
      REX_STORE_U8(device + 0x2B3F, uint8_t(REX_LOAD_U8(device + 0x2B3F) | 0x80));
    }
    REX_STORE_U8(query + 0x10, 2);
    return;
  }
  if (!(flags & 1)) return;
  if (type != 8) {
    if (type == 9) {
      const uint32_t slot = LoadGuestU32(query + 4 * (LoadGuestU32(query + 0x94) + 6));
      const uint8_t query_flags = REX_LOAD_U8(query + 0x14);
      if (!LoadGuestU32(query + 0x18) || (query_flags & 0x80)) {
        REX_STORE_U8(query + 0x14, uint8_t(query_flags & 0x7F));
        StoreGuestU32(slot + 0x10, 0xFFFFFEEDu);
        StoreGuestU32(slot + 0x14, 0xFFFFFEEDu);
      }
      OrGuestU64(device + 0x10, 0x0300000000000008ull);
    } else if (type == 10) {
      if (REX_LOAD_U8(device + 0x2B40) & 0x80) ResetScreenExtentState(device);
      REX_STORE_U8(device + 0x2B3F, uint8_t(REX_LOAD_U8(device + 0x2B3F) & 0x7F));
    }
    REX_STORE_U8(query + 0x10, 1);
  }
  StoreGuestU32(query + 0x18, LoadGuestU32(device + kCpuFence));
  REX_STORE_U8(query + 0x14, uint8_t(REX_LOAD_U8(query + 0x14) | 0x40));
}

void D3DDevice_SetPredication_hook(uint32_t device, uint32_t mask) {
  uint8_t* base = GuestBase();
  uint8_t state = REX_LOAD_U8(device + 0x2B3C);
  if (mask) {
    StoreGuestU32(device + 0x331C, mask);
    StoreGuestU32(device + 0x3324, 1);
    state &= 0xFE;
  } else {
    StoreGuestU32(device + 0x3324, 0);
    bool on = false;
    if (!(state & 0x0C) && !REX_LOAD_U8(device + 0x3113)) {
      if (state & 0x10) {
        on = true;
      } else if (state & 0x20) {
        on = true;
        for (uint32_t i = 0; i < 5; ++i) {
          const uint32_t bound = LoadGuestU32(device + 0x3210 + 4 * i);
          if (bound && bound != LoadGuestU32(device + 0x3330 + 4 * i)) on = false;
        }
      }
    }
    state = uint8_t((state & 0xFE) | (on ? 1 : 0));
    StoreGuestU32(device + 0x331C, 0xFFFFFFFFu);
  }
  REX_STORE_U8(device + 0x2B3C, state);
  StoreGuestU32(device + 0x3320, 0);
  StoreGuestU32(device + 0x2B34, mask);
  StoreGuestU32(device + 0x2B38, 0);
}

void D3DDevice_FlushHiZStencil_hook(uint32_t device) {
  uint8_t* base = GuestBase();
  REX_STORE_U8(device + 0x2B3D, uint8_t(REX_LOAD_U8(device + 0x2B3D) & 0xFB));
}

// UE3's tiled DrawPrimitiveUP asks whether its vertices fit; they go to the renderer's own vertex sink.
void D3DDevice_QueryBufferSpace_hook(uint32_t device, uint32_t used_out, uint32_t remaining_out) {
  (void)device;
  StoreGuestU32(used_out, 0);
  StoreGuestU32(remaining_out, 16u << 20);
}

// UE3 sends its tessellated quad patches here; translated shaders have no tessellation stage, so they are dropped and reported.
void D3DDevice_DrawTessellatedVertices_hook(uint32_t device, uint32_t primitive_type, uint32_t start_vertex, uint32_t patch_count) {
  (void)device;
  static std::atomic<uint32_t> reported{0};
  if (reported++ < 4) REXGPU_WARN("[gfx] tessellated draw dropped: primitive {:#x}, start {}, {} patches", primitive_type, start_vertex, patch_count);
}

void D3D_BeginMemExport_hook(uint32_t device, uint32_t resource) {
  const uint32_t type = LoadGuestU32(resource) & 0xF;
  uint32_t address = LoadGuestU32(resource + 0x18);
  if (type == 1) {
    address &= ~3u;
  } else if (type == 3) {
    address = LoadGuestU32(resource + 0x20) & ~0xFFFu;
    if (!address) address = LoadGuestU32(resource + 0x30) & ~0xFFFu;
  }
  StoreGuestU32(device + 0x2898, GuestPhysicalAddress(address));
  OrGuestU64(device + 0x10, 1ull << 51);
  if (!LoadGuestU32(device + 0x3650)) OrGuestU64(device + 0x10, 0x80000ull | (7ull << 39) | 8);
  StoreGuestU32(device + 0x3658, 0);
  StoreGuestU32(device + 0x3654, 0);
  StoreGuestU32(device + 0x3650, LoadGuestU32(device + 0x3650) + 1);
}

uint32_t D3D_EndMemExport_hook(uint32_t device, uint32_t resource) {
  StoreGuestU32(device + 0x2898, 0);
  if (const uint32_t fence = LoadGuestU32(device + kCpuFence)) {
    StoreGuestU32(resource + 8, fence);
    StoreGuestU32(resource + 0xC, fence);
  }
  const uint32_t depth = LoadGuestU32(device + 0x3650) - 1;
  StoreGuestU32(device + 0x3650, depth);
  if (depth) {
    OrGuestU64(device + 0x10, 0x80000ull | (7ull << 39) | 8);
  } else {
    OrGuestU64(device + 0x10, (7ull << 39) | (1ull << 44) | (1ull << 37) | 0x100000 | 0x80000 | 0x800 | 0x100 | 0xE0 | 8);
    OrGuestU64(device + 0x18, (1ull << 35) | (1ull << 54));
    OrGuestU64(device + 0x20, 0x80000ull | (1ull << 35));
  }
  StoreGuestU32(device + 0x3654, 0);
  return LoadGuestU32(device + 0x30);
}

}  // namespace

// D3DResource_AddRef(D3DResource*) -> ULONG
REX_HOOK(D3DResource_AddRef, D3DResource_AddRef_hook)
// D3DResource_Release(D3DResource*) -> ULONG
REX_HOOK(D3DResource_Release, D3DResource_Release_hook)
// D3DResource_GetDevice(D3DResource*, D3DDevice** ppDevice)
REX_HOOK(D3DResource_GetDevice, D3DResource_GetDevice_hook)
// D3DResource_GetType(D3DResource*) -> D3DRESOURCETYPE
REX_HOOK(D3DResource_GetType, D3DResource_GetType_hook)
// D3DResource_IsSet(D3DResource*, D3DDevice*) -> BOOL
REX_HOOK(D3DResource_IsSet, D3DResource_IsSet_hook)
// D3D_LockResource(D3DResource*, DWORD Reason, BOOL IsMip, DWORD BaseAddress, DWORD MipAddress, DWORD LockAddress, DWORD LockSize, DWORD Flags, [stack] DWORD Coherency) -> void*
REX_HOOK(D3D_LockResource, D3D_LockResource_hook)
// D3D_UnlockResource(D3DResource*, DWORD BaseAddress, DWORD MipAddress)
REX_HOOK(D3D_UnlockResource, D3D_UnlockResource_hook)
// D3D_FlushCachedRange(DWORD Start, DWORD End) -> Start
REX_HOOK(D3D_FlushCachedRange, D3D_FlushCachedRange_hook)
// D3D_InvalidateGpuCacheRange(DWORD Address, DWORD Size)
REX_HOOK(D3D_InvalidateGpuCacheRange, D3D_InvalidateGpuCacheRange_hook)
// D3DDevice_Release(D3DDevice*) -> ULONG
REX_HOOK(D3DDevice_Release, D3DDevice_Release_hook)
// D3DDevice_GetDeviceCaps(D3DDevice*, D3DCAPS9*)
REX_HOOK(D3DDevice_GetDeviceCaps, D3DDevice_GetDeviceCaps_hook)
// D3DDevice_CreateOcclusionQuery(D3DDevice*) -> D3DQuery*
REX_HOOK(D3DDevice_CreateOcclusionQuery, D3DDevice_CreateOcclusionQuery_hook)
// D3DQuery_Release(D3DQuery*) -> ULONG
REX_HOOK(D3DQuery_Release, D3DQuery_Release_hook)
// D3DQuery_Issue(D3DQuery*, DWORD IssueFlags)
REX_HOOK(D3DQuery_Issue, D3DQuery_Issue_hook)
// D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask)
REX_HOOK(D3DDevice_SetPredication, D3DDevice_SetPredication_hook)
// D3DDevice_FlushHiZStencil(D3DDevice*)
REX_HOOK(D3DDevice_FlushHiZStencil, D3DDevice_FlushHiZStencil_hook)
// D3DDevice_QueryBufferSpace(D3DDevice*, DWORD* pUsed, DWORD* pRemaining)
REX_HOOK(D3DDevice_QueryBufferSpace, D3DDevice_QueryBufferSpace_hook)
// D3DDevice_DrawTessellatedVertices(D3DDevice*, DWORD PrimitiveType, UINT StartVertex, UINT PatchCount)
REX_HOOK(D3DDevice_DrawTessellatedVertices, D3DDevice_DrawTessellatedVertices_hook)
// D3D_BeginMemExport(D3DDevice*, D3DResource*)
REX_HOOK(D3D_BeginMemExport, D3D_BeginMemExport_hook)
// D3D_EndMemExport(D3DDevice*, D3DResource*) -> DWORD
REX_HOOK(D3D_EndMemExport, D3D_EndMemExport_hook)
