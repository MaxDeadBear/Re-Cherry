#include "gpu/hooks/hooks_internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "gpu/guest_device.h"
#include "gpu/guest_memory.h"

// Nothing consumes the guest ring. SetRingBufferParameters lays it over guest memory nothing reads, sized so no frame reaches its limit, and Swap rewinds it; the D3D ring internals are then never reached and are REX_STUBs.

namespace {

struct GuestSink {
  uint32_t base = 0;
  uint32_t size = 0;
  std::atomic<uint32_t> used{0};
};

// One block: command packets grow up from the base, ring data is carved down from the top, and the carver refuses to pass the write pointer.
constexpr uint32_t kCommandBytes = 4u << 20;
constexpr uint32_t kRingDataBytes = 12u << 20;
// Vertices for this renderer's own Begin*Vertices hooks.
constexpr uint32_t kDataSinkBytes = 16u << 20;

GuestSink g_ring;
GuestSink g_data_sink;
uint32_t g_allocator = 0;
std::atomic<uint32_t> g_peak_command_bytes{0};
std::atomic<uint32_t> g_peak_data_bytes{0};

uint32_t AllocPhysical(rex::memory::BaseHeap* heap, uint32_t bytes) {
  uint32_t address = 0;
  if (!heap || !heap->Alloc(bytes, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) return 0;
  return address;
}

void NotePeak(std::atomic<uint32_t>& peak, uint32_t value) {
  uint32_t seen = peak.load(std::memory_order_relaxed);
  while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

}  // namespace

namespace cherry::gpu::hooks {

uint32_t GuestDevice() {
  return LoadGuestU32(kGDirect3DDevice);
}

bool RingSinksReady() {
  static std::once_flag once;
  std::call_once(once, [] {
    rex::memory::BaseHeap* heap = rex::system::kernel_state()->memory()->LookupHeapByType(true, 4096);
    g_ring.base = AllocPhysical(heap, kCommandBytes + kRingDataBytes);
    g_ring.size = kCommandBytes + kRingDataBytes;
    g_data_sink.base = AllocPhysical(heap, kDataSinkBytes);
    g_data_sink.size = kDataSinkBytes;
    g_allocator = AllocPhysical(heap, 4096);
    if (g_ring.base && g_allocator) {
      std::memset(GuestBase() + g_allocator, 0, dev::kAllocatorBytes);
      StoreGuestU32(g_allocator + dev::kAllocatorBufferBase, g_ring.base + kCommandBytes);
      StoreGuestU32(g_allocator + dev::kAllocatorBufferSize, kRingDataBytes);
    }
    if (g_ring.base && g_data_sink.base && g_allocator) REXGPU_INFO("[gfx] guest D3D ring at {:08X}: {} MB of commands and {} MB of ring data, never read; vertex sink {:08X} ({} MB per frame)", g_ring.base, kCommandBytes >> 20, kRingDataBytes >> 20, g_data_sink.base, kDataSinkBytes >> 20);
    else REXGPU_ERROR("[gfx] no physical memory for the guest D3D ring");
  });
  return g_ring.base && g_data_sink.base && g_allocator;
}

uint32_t DataSinkAlloc(uint32_t bytes, uint32_t alignment) {
  alignment = std::max(alignment, 16u);
  bytes = std::max(bytes, 16u);
  uint32_t used = g_data_sink.used.load(std::memory_order_relaxed);
  for (;;) {
    uint32_t offset = (used + alignment - 1) & ~(alignment - 1);
    if (offset + bytes > g_data_sink.size) {
      static std::atomic<uint32_t> warned{0};
      if (warned.fetch_add(1, std::memory_order_relaxed) < 4) REXGPU_WARN("[gfx] guest vertex sink full ({} MB in one frame); wrapping", kDataSinkBytes >> 20);
      offset = 0;
    }
    if (g_data_sink.used.compare_exchange_weak(used, offset + bytes, std::memory_order_relaxed)) return g_data_sink.base + offset;
  }
}

void ResetDataSink() {
  g_data_sink.used.store(0, std::memory_order_relaxed);
}

uint32_t RewindCommandRing(uint32_t device) {
  uint8_t* base = GuestBase();
  const uint32_t write = LoadGuestU32(device + dev::kCmdWritePtr) + 4;
  if (write >= g_ring.base && write <= g_ring.base + kCommandBytes) NotePeak(g_peak_command_bytes, write - g_ring.base);
  if (const uint32_t cursor = LoadGuestU32(device + dev::kSecondaryDataCursor)) NotePeak(g_peak_data_bytes, g_ring.base + g_ring.size - cursor);
  const uint32_t start = g_ring.base - 4;
  const uint32_t limit = g_ring.base + kCommandBytes - dev::kSegmentTailBytes;
  StoreGuestU32(device + dev::kSegmentStart, g_ring.base);
  StoreGuestU32(device + dev::kCmdWritePtr, start);
  StoreGuestU32(device + dev::kCmdHardEndPtr, limit);
  StoreGuestU32(device + dev::kCmdSoftEndPtr, limit - dev::kSoftEndGapBytes);
  StoreGuestU32(device + dev::kSecondaryDataCursor, 0);
  StoreGuestU32(device + dev::kSegmentDataBytes, 0);
  REX_STORE_U8(device + dev::kRingFlags, uint8_t(REX_LOAD_U8(device + dev::kRingFlags) & ~0x20));
  return start;
}

void LogRingStats(uint32_t device) {
  const uint32_t commands = g_peak_command_bytes.exchange(0);
  const uint32_t data = g_peak_data_bytes.exchange(0);
  REXGPU_INFO("[gfx] guest ring: peak {} KB of commands and {} KB of ring data in a frame", commands >> 10, data >> 10);
  if (commands > kCommandBytes / 2 || data > kRingDataBytes / 2) REXGPU_WARN("[gfx] guest ring over half full in one frame; a frame that fills it would reach the stubbed KickOff");
  if (LoadGuestU32(device + dev::kCustomRingAllocator) != g_allocator) REXGPU_WARN("[gfx] the game replaced the ring allocator; its ring data no longer comes from the fixed buffer");
}

void PublishFences(uint32_t device) {
  if (!device) return;
  const uint32_t readback = LoadGuestU32(device + dev::kGpuReadbackPtr);
  if (readback) StoreGuestU32(readback, LoadGuestU32(device + dev::kCpuFence));
}

}  // namespace cherry::gpu::hooks

namespace {

using namespace cherry::gpu;
using namespace cherry::gpu::hooks;

// Lays the ring out as the original does (0x829EA1F0), over the fixed block instead of a primary and a segmented secondary ring, and skips the Vd* calls that start a GPU.
uint32_t D3DDevice_SetRingBufferParameters_hook(uint32_t device, uint32_t parameters) {
  uint8_t* base = GuestBase();
  if (!parameters) {
    StoreGuestU32(device + dev::kCmdWritePtr, 0);
    StoreGuestU32(device + dev::kCmdHardEndPtr, 0);
    return 0;
  }
  if (!RingSinksReady()) return 0x8007000Eu;
  StoreGuestU32(device + dev::kCustomRingAllocator, g_allocator);
  StoreGuestU32(device + dev::kPrimaryBase, 0);
  StoreGuestU32(device + dev::kPrimaryRingMask, 0);
  StoreGuestU32(device + dev::kPrimaryWriteIndex, 0);
  StoreGuestU32(device + dev::kSecondaryBase, g_ring.base);
  StoreGuestU32(device + dev::kSecondaryStart, g_ring.base);
  StoreGuestU32(device + dev::kSecondaryLast, g_ring.base + kCommandBytes - 4);
  StoreGuestU32(device + dev::kSegmentSize, kCommandBytes);
  StoreGuestU32(device + dev::kSegmentDataLimit, 0xFFFFFFFFu);
  REX_STORE_U16(device + dev::kGprAllocation, 1);
  REX_STORE_U16(device + dev::kGprAllocation + 2, 0x800);
  const uint32_t write = RewindCommandRing(device);
  if (!LoadGuestU32(device + dev::kCpuFence)) StoreGuestU32(device + dev::kCpuFence, 3);
  if (const uint32_t readback = LoadGuestU32(device + dev::kGpuReadbackPtr)) {
    StoreGuestU32(readback, LoadGuestU32(device + dev::kCpuFence) - 2);
    StoreGuestU32(readback + dev::kReadbackRingPtr, write | (LoadGuestU32(device + dev::kRingPredicationBits) & 3));
    StoreGuestU32(readback + dev::kReadbackRPtr, 0);
  }
  REXGPU_INFO("[gfx] guest D3D ring laid over {:08X}", g_ring.base);
  return 0;
}

}  // namespace

// D3DDevice_SetRingBufferParameters(D3DDevice*, const D3DRING_BUFFER_PARAMETERS*) -> HRESULT
REX_HOOK(D3DDevice_SetRingBufferParameters, D3DDevice_SetRingBufferParameters_hook)
// D3D_RingBufferDeviceAllocate(D3DDevice*, DWORD flags, DWORD* pSize, DWORD alignment) -> void*; ring data comes from the fixed allocator buffer
REX_STUB(D3D_RingBufferDeviceAllocate)
// D3D_CDevice_KickOff(D3DDevice*) -> DWORD*; UE3's flushes call it and ignore the result, and the limit it serves is never reached
REX_STUB(D3D_CDevice_KickOff)
// D3D_CDevice_BlockOnPrimaryRange(D3DDevice*, DWORD position, DWORD size) -> DWORD; only KickOff and WritePrimary reach it
REX_STUB(D3D_CDevice_BlockOnPrimaryRange)
// D3D_CDevice_WritePrimary(D3DDevice*, const DWORD* packets, DWORD count); there is no primary ring
REX_STUB(D3D_CDevice_WritePrimary)
