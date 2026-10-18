// pipelines.bin: every draw pipeline the game has needed, so the next launch builds them on background threads before the game asks, instead of stalling the frame that first draws with one.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <plume_render_interface.h>

#include <rex/logging.h>

#include "gpu/graphics_system.h"

namespace cherry::gpu {

namespace {

constexpr char kMagic[4] = {'C', 'P', 'L', 'C'};
constexpr uint32_t kVersion = 1;

struct FileHeader {
  char magic[4];
  uint32_t version;
  uint32_t record_size;
};

std::vector<PipelineState> g_unwritten;

void Append(const std::filesystem::path& path, const std::vector<PipelineState>& states) {
  std::error_code ec;
  const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) < sizeof(FileHeader);
  std::ofstream out(path, std::ios::binary | (fresh ? std::ios::trunc : std::ios::app));
  if (!out) return;
  if (fresh) {
    FileHeader header;
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kVersion;
    header.record_size = sizeof(PipelineState);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  }
  out.write(reinterpret_cast<const char*>(states.data()), std::streamsize(states.size() * sizeof(PipelineState)));
}

std::vector<PipelineState> Load(const std::filesystem::path& path) {
  std::vector<PipelineState> states;
  std::ifstream in(path, std::ios::binary);
  FileHeader header;
  if (!in || !in.read(reinterpret_cast<char*>(&header), sizeof(header)) || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kVersion || header.record_size != sizeof(PipelineState)) return states;
  PipelineState state;
  while (in.read(reinterpret_cast<char*>(&state), sizeof(state))) states.push_back(state);
  return states;
}

}  // namespace

uint64_t GraphicsSystem::PipelineKey(const PipelineState& state) {
  return XXH3_64bits(&state, sizeof(state));
}

void GraphicsSystem::SetPipelineCachePath(const std::filesystem::path& path) {
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_cache_path_ = path;
  }
  StartPipelinePrecompile();
}

void GraphicsSystem::RecordPipeline(const PipelineState& state) {
  std::lock_guard<std::mutex> lock(pipeline_mutex_);
  if (!recorded_pipelines_.insert(PipelineKey(state)).second) return;
  // Pipelines built before the path is known (the boot logos) are written with the first one after.
  g_unwritten.push_back(state);
  if (pipeline_cache_path_.empty()) return;
  Append(pipeline_cache_path_, g_unwritten);
  g_unwritten.clear();
}

void GraphicsSystem::StartPipelinePrecompile() {
  std::vector<PipelineState> states;
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    if (precompile_started_ || pipeline_cache_path_.empty() || !draw_ready_) return;
    precompile_started_ = true;
    for (const PipelineState& state : Load(pipeline_cache_path_)) {
      const uint64_t key = PipelineKey(state);
      if (recorded_pipelines_.insert(key).second && !pipelines_.count(key)) states.push_back(state);
    }
  }
  if (states.empty()) return;
  REXGPU_INFO("[gfx] building {} pipelines from {} in the background", states.size(), pipeline_cache_path_.filename().string());
  auto shared = std::make_shared<std::vector<PipelineState>>(std::move(states));
  auto next = std::make_shared<std::atomic<size_t>>(0);
  auto running = std::make_shared<std::atomic<uint32_t>>(0);
  const auto start = std::chrono::steady_clock::now();
  const uint32_t workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
  running->store(workers);
  for (uint32_t w = 0; w < workers; ++w) {
    precompile_threads_.emplace_back([this, shared, next, running, start] {
      uint32_t built = 0;
      for (size_t i = next->fetch_add(1); i < shared->size() && !precompile_stop_.load(); i = next->fetch_add(1)) {
        const PipelineState& state = (*shared)[i];
        const uint64_t key = PipelineKey(state);
        {
          std::lock_guard<std::mutex> lock(pipeline_mutex_);
          if (pipelines_.count(key)) continue;
        }
        std::unique_ptr<plume::RenderPipeline> pipeline = CreatePipeline(state);
        if (!pipeline) continue;
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        if (pipelines_.try_emplace(key, std::move(pipeline)).second) ++built;
      }
      if (running->fetch_sub(1) == 1 && !precompile_stop_.load()) REXGPU_INFO("[gfx] background pipeline build done in {:.1f} s", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    });
  }
}

void GraphicsSystem::StopPipelinePrecompile() {
  precompile_stop_.store(true);
  for (std::thread& thread : precompile_threads_) {
    if (thread.joinable()) thread.join();
  }
  precompile_threads_.clear();
}

}  // namespace cherry::gpu
