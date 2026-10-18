#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace cherry::gpu {
struct HostLayout;
}  // namespace cherry::gpu

namespace cherry::gpu::debug {

bool BadVertexCheckEnabled();
// With --cherry_debug_bad_vertices, logs draws whose decoded positions are NaN, infinite, denormal or beyond 1e7; `describe` says where the draw read them from.
void CheckVertices(const HostLayout& layout, const uint8_t* vertices, uint32_t count, const std::function<std::string()>& describe);

}  // namespace cherry::gpu::debug
