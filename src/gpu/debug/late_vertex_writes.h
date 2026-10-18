#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace cherry::gpu::debug {

bool LateVertexWritesEnabled();
// With --cherry_debug_late_vertex_writes, remembers the guest bytes a draw read when it was captured.
void NoteDrawBytes(uint64_t vs_hash, uint32_t first_address, const std::vector<std::pair<const uint8_t*, size_t>>& ranges);
// At the frame's end: logs, every 150 frames, the draws whose vertex or index bytes the guest changed after the draw call.
void CheckDrawBytes();

}  // namespace cherry::gpu::debug
