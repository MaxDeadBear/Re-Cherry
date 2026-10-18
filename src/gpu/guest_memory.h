// Guest-memory access for the typed REX_HOOK functions, which are not handed `base`; REX_LOAD_* and REX_STORE_* expect it in scope.
#pragma once

#include <bit>
#include <cstdint>

#include "re_cherry_init.h"

#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace cherry::gpu {

inline uint8_t* GuestBase() {
  return rex::system::kernel_state()->memory()->virtual_membase();
}

inline uint32_t LoadGuestU32(uint32_t address) {
  uint8_t* base = GuestBase();
  return REX_LOAD_U32(address);
}

inline void StoreGuestU32(uint32_t address, uint32_t value) {
  uint8_t* base = GuestBase();
  REX_STORE_U32(address, value);
}

inline void OrGuestU32(uint32_t address, uint32_t bits) {
  StoreGuestU32(address, LoadGuestU32(address) | bits);
}

inline float LoadGuestF32(uint32_t address) {
  return std::bit_cast<float>(LoadGuestU32(address));
}

inline void StoreGuestF32(uint32_t address, float value) {
  StoreGuestU32(address, std::bit_cast<uint32_t>(value));
}

// m_Pending masks are big-endian u64s: the high word first.
inline void OrGuestU64(uint32_t address, uint64_t bits) {
  OrGuestU32(address, uint32_t(bits >> 32));
  OrGuestU32(address + 4, uint32_t(bits));
}

// D3D's virtual-to-physical form for GPU addresses: drop the view bits and add the page the 0xE0000000 view is offset by.
inline uint32_t GuestPhysicalAddress(uint32_t address) {
  return (address & 0x1FFFFFFF) + (((address >> 20) + 0x200) & 0x1000);
}

}  // namespace cherry::gpu
