#include "gpu/guest_writes.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace cherry::gpu {

namespace {

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPhysicalBytes = 0x20000000;
constexpr uint32_t kPages = kPhysicalBytes >> kPageShift;

// Per 4 KB physical page, the stamp of its last write; stamps only grow, so a range was written after `stamp` if any page's is at least it (a write racing the arm counts).
std::unique_ptr<std::atomic<uint64_t>[]> g_page_stamps;
std::atomic<uint64_t> g_stamp{1};
std::atomic<bool> g_ready{false};

// On the writing thread, with the SDK's global lock held: the page is unwatched, so only its own page is returned for unprotecting.
std::pair<uint32_t, uint32_t> OnInvalidated(void*, uint32_t start, uint32_t length, bool) {
  const uint32_t first = std::min(start >> kPageShift, kPages);
  const uint32_t end = std::min(uint32_t((uint64_t(start) + length + (1u << kPageShift) - 1) >> kPageShift), kPages);
  const uint64_t stamp = g_stamp.fetch_add(1, std::memory_order_relaxed) + 1;
  for (uint32_t page = first; page < end; ++page) g_page_stamps[page].store(stamp, std::memory_order_release);
  return {first << kPageShift, (end - first) << kPageShift};
}

}  // namespace

void InitGuestWriteWatch() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto* memory = rex::system::kernel_state() ? rex::system::kernel_state()->memory() : nullptr;
    if (!memory) return;
    g_page_stamps = std::make_unique<std::atomic<uint64_t>[]>(kPages);
    memory->RegisterPhysicalMemoryInvalidationCallback(OnInvalidated, nullptr);
    g_ready.store(true, std::memory_order_release);
  });
}

uint64_t ArmWriteWatch(uint32_t physical, uint32_t length) {
  if (!g_ready.load(std::memory_order_acquire) || !length || uint64_t(physical) + length > kPhysicalBytes) return 0;
  const uint64_t stamp = g_stamp.load(std::memory_order_acquire);
  rex::system::kernel_state()->memory()->EnablePhysicalMemoryAccessCallbacks(physical, length, true, false);
  return stamp;
}

bool WrittenSince(uint32_t physical, uint32_t length, uint64_t stamp) {
  if (!g_ready.load(std::memory_order_acquire) || !stamp || uint64_t(physical) + length > kPhysicalBytes) return true;
  const uint32_t first = physical >> kPageShift;
  const uint32_t end = uint32_t((uint64_t(physical) + length + (1u << kPageShift) - 1) >> kPageShift);
  for (uint32_t page = first; page < end; ++page) {
    if (g_page_stamps[page].load(std::memory_order_acquire) >= stamp) return true;
  }
  return false;
}

}  // namespace cherry::gpu
