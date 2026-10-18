// The game's Flash movies as they load: the options screen is given its GRAPHICS pages (option_menu.h).

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "ui/option_menu.h"

namespace {

// The memory file FGFxFileOpener::OpenFile returns for a movie in a package: data at +8, length at +12.
constexpr uint32_t kMemoryFileVtable = 0x8218A6E8;

struct Patched {
  uint32_t address = 0;  // 0: the movie is used as it is
  uint32_t size = 0;
};

std::mutex g_mutex;
// By the original movie's hash and size, so a movie is rewritten once however often it is opened.
std::unordered_map<uint64_t, Patched> g_patched;

Patched Patch(const uint8_t* bytes, uint32_t size) {
  std::vector<uint8_t> movie = cherry::ui::AddGraphicsPage(std::vector<uint8_t>(bytes, bytes + size));
  if (movie.empty()) return {};
  REXLOG_INFO("[ui] options screen movie given the GRAPHICS pages ({} -> {} bytes)", size, movie.size());
  const uint32_t address = rex::system::kernel_state()->memory()->SystemHeapAlloc(uint32_t(movie.size()));
  if (!address) return {};
  std::memcpy(rex::system::kernel_state()->memory()->virtual_membase() + address, movie.data(), movie.size());
  return {address, uint32_t(movie.size())};
}

void SwapMovie(uint8_t* base, uint32_t file) {
  const uint32_t data = REX_LOAD_U32(file + 8);
  const uint32_t size = REX_LOAD_U32(file + 12);
  if (!data || size < 9 || std::memcmp(base + data, "GFX", 3) != 0) return;
  const uint64_t key = XXH3_64bits(base + data, size) ^ size;
  std::lock_guard lock(g_mutex);
  auto [found, inserted] = g_patched.try_emplace(key);
  if (inserted) found->second = Patch(base + data, size);
  if (!found->second.address) return;
  REX_STORE_U32(file + 8, found->second.address);
  REX_STORE_U32(file + 12, found->second.size);
}

}  // namespace

// FGFxFileOpener::OpenFile(this, url)
REX_HOOK_RAW(sub_82932070) {
  __imp__sub_82932070(ctx, base);
  const uint32_t file = ctx.r3.u32;
  if (file && REX_LOAD_U32(file) == kMemoryFileVtable) SwapMovie(base, file);
}
