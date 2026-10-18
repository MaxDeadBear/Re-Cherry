// Which pages of guest physical memory the guest has written, through the SDK's one-shot write watches, so caches skip re-hashing bytes nothing touched.
#pragma once

#include <cstdint>

namespace cherry::gpu {

// Registers the invalidation callback; call once the kernel's memory exists.
void InitGuestWriteWatch();

// Watches [physical, physical + length) and returns the stamp to pass to WrittenSince for it; take it before reading the bytes.
uint64_t ArmWriteWatch(uint32_t physical, uint32_t length);

// Whether any page of the range was written, released or re-allocated after ArmWriteWatch returned `stamp` for it.
bool WrittenSince(uint32_t physical, uint32_t length, uint64_t stamp);

// Guest virtual addresses in the 0xA0000000 and 0xC0000000 views of physical memory, the ones writes to which are watched; ~0 for any other.
inline uint32_t WatchablePhysical(uint32_t virtual_address) {
  return virtual_address >= 0xA0000000u && virtual_address < 0xE0000000u ? virtual_address & 0x1FFFFFFFu : ~0u;
}

}  // namespace cherry::gpu
