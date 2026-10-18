#include "gpu/hooks/hooks_internal.h"

#include <rex/hook.h>

#include "gpu/guest_memory.h"

// The original waits for GPU-written ZPASS counters, which stay at their -275 sentinel with no guest GPU, so UE3's occlusion fetch spun forever on the rendering thread.

namespace {

using namespace cherry::gpu;

constexpr uint32_t kQueryType = 0x4;
constexpr uint32_t kQueryFlags = 0x14;
constexpr uint32_t kQueryIssuedFlag = 0x40;
constexpr uint32_t kQueryResultReadFlag = 0x80;
constexpr uint32_t kQueryTypeOcclusion = 9;
constexpr uint32_t kQueryTypeScreenExtent = 10;
constexpr uint32_t kScreenExtentBytes = 24;
constexpr uint32_t kNotIssued = 0x8876086A;
// Until the draw path runs real occlusion queries, everything counts as visible.
constexpr uint32_t kVisibleSamples = 1u << 16;

uint32_t D3DQuery_GetData_hook(uint32_t query, uint32_t data) {
  uint8_t* base = GuestBase();
  const uint8_t flags = REX_LOAD_U8(query + kQueryFlags);
  if (!(flags & kQueryIssuedFlag)) return kNotIssued;
  const uint32_t type = REX_LOAD_U32(query + kQueryType);
  if (type == kQueryTypeOcclusion) {
    if (data) REX_STORE_U32(data, kVisibleSamples);
    REX_STORE_U8(query + kQueryFlags, (flags & 0x7F) | kQueryResultReadFlag);
  } else if (type == kQueryTypeScreenExtent && data) {
    for (uint32_t i = 0; i < kScreenExtentBytes; i += 4) REX_STORE_U32(data + i, 0);
  }
  return 0;
}

}  // namespace

// D3DQuery_GetData(D3DQuery*, void* pData) -> HRESULT (S_OK, S_FALSE while pending, D3DERR_INVALIDCALL if never issued)
REX_HOOK(D3DQuery_GetData, D3DQuery_GetData_hook)
