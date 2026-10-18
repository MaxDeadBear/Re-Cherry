#include "gpu/debug/renderdoc.h"

#include <charconv>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <renderdoc/renderdoc_app.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(cherry_debug_renderdoc_frames, "", "Lollipop/Debug", "Comma-separated presented frame numbers to capture when the game runs under RenderDoc (renderdoccmd capture); empty is off.");
REXCVAR_DEFINE_STRING(cherry_debug_renderdoc_on_shader, "", "Lollipop/Debug", "Under RenderDoc, capture the frame after the first one that draws with this vertex shader hash (hex, as the frame trace prints it), and once more 8 frames later.");

namespace cherry::gpu::debug {

namespace {

RENDERDOC_API_1_0_1* Api() {
  static RENDERDOC_API_1_0_1* api = [] {
    RENDERDOC_API_1_0_1* found = nullptr;
#ifdef _WIN32
    // Only present when RenderDoc injected itself; the game never loads it.
    if (HMODULE module = GetModuleHandleA("renderdoc.dll")) {
      auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
      if (get_api && get_api(eRENDERDOC_API_Version_1_0_1, reinterpret_cast<void**>(&found)) == 1) REXGPU_INFO("[gfx] RenderDoc attached");
    }
#endif
    return found;
  }();
  return api;
}

std::set<uint64_t> RequestedFrames() {
  std::set<uint64_t> frames;
  const std::string list = REXCVAR_GET(cherry_debug_renderdoc_frames);
  for (size_t start = 0; start < list.size();) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    uint64_t frame = 0;
    if (std::from_chars(list.data() + start, list.data() + end, frame).ec == std::errc()) frames.insert(frame);
    start = end + 1;
  }
  return frames;
}

}  // namespace

uint64_t RenderDocTriggerShader() {
  static const uint64_t hash = [] {
    const std::string text = REXCVAR_GET(cherry_debug_renderdoc_on_shader);
    uint64_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return value;
  }();
  return hash;
}

void RenderDocShaderSeen(uint64_t presented_frame) {
  static uint32_t armed = 0;
  static uint64_t last = 0;
  if (!Api() || armed >= 2 || (armed && presented_frame < last + 8)) return;
  Api()->TriggerCapture();
  last = presented_frame;
  ++armed;
  REXGPU_INFO("[gfx] RenderDoc capture armed after presented frame {} drew the requested shader", presented_frame);
}

bool RenderDocBeginDeviceCapture(void* device, void* window) {
  if (!Api()) return false;
  Api()->StartFrameCapture(device, window);
  return true;
}

void RenderDocEndDeviceCapture(void* device, void* window) {
  if (Api() && Api()->EndFrameCapture(device, window)) REXGPU_INFO("[gfx] RenderDoc captured the requested frame");
}

void RenderDocBeforePresent(uint64_t presented_frame) {
  static const std::set<uint64_t> frames = RequestedFrames();
  if (frames.empty() || !Api()) return;
  // TriggerCapture records the frame after the next present, so it is armed one present early.
  if (frames.count(presented_frame + 1)) {
    Api()->TriggerCapture();
    REXGPU_INFO("[gfx] RenderDoc capture armed for presented frame {}", presented_frame + 1);
  }
}

}  // namespace cherry::gpu::debug
