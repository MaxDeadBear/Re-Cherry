#include "gpu/debug/late_vertex_writes.h"

#include <map>

#include <rex/cvar.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(cherry_debug_late_vertex_writes, false, "Lollipop/Debug", "Log, every 150 frames, the draws whose vertex or index bytes the guest changed after the draw call and before the present.");

namespace cherry::gpu::debug {

namespace {

struct Noted {
  uint64_t vs_hash;
  uint32_t first_address;
  std::vector<std::pair<const uint8_t*, size_t>> ranges;
  uint64_t hash;
};

std::vector<Noted> g_noted;
std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_changed;
uint32_t g_frames = 0;
uint32_t g_draws = 0;

uint64_t Hash(const std::vector<std::pair<const uint8_t*, size_t>>& ranges) {
  uint64_t hash = 0;
  for (const auto& [data, size] : ranges) hash = XXH3_64bits_withSeed(data, size, hash);
  return hash;
}

}  // namespace

bool LateVertexWritesEnabled() {
  return REXCVAR_GET(cherry_debug_late_vertex_writes);
}

void NoteDrawBytes(uint64_t vs_hash, uint32_t first_address, const std::vector<std::pair<const uint8_t*, size_t>>& ranges) {
  g_noted.push_back({vs_hash, first_address, ranges, Hash(ranges)});
}

void CheckDrawBytes() {
  if (!LateVertexWritesEnabled()) return;
  for (const Noted& noted : g_noted) {
    ++g_draws;
    if (Hash(noted.ranges) != noted.hash) ++g_changed[{noted.vs_hash, noted.first_address}];
  }
  g_noted.clear();
  if (++g_frames < 150) return;
  uint32_t total = 0;
  for (const auto& [key, count] : g_changed) total += count;
  REXGPU_INFO("[late writes] {} of {} draws over 150 frames read bytes the guest changed before the present", total, g_draws);
  uint32_t shown = 0;
  for (const auto& [key, count] : g_changed) {
    if (shown++ >= 16) break;
    REXGPU_INFO("[late writes]   VS {:016X} stream at {:08X}: {} draws", key.first, key.second, count);
  }
  g_changed.clear();
  g_frames = 0;
  g_draws = 0;
}

}  // namespace cherry::gpu::debug
