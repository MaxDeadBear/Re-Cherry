#include "engine/debug/scripted_input.h"

#include <charconv>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>

#include "engine/frame_rate.h"

REXCVAR_DEFINE_STRING(cherry_debug_press, "", "Lollipop/Debug", "Buttons pad 0 presses at given presented frames, as frame:button[+button],... (start, back, a, b, x, y, up, down, left, right, lb, rb); each is held for 6 frames. Empty is off.");

namespace cherry::engine::debug {

namespace {

constexpr uint64_t kHoldFrames = 6;

struct Press {
  uint64_t frame;
  uint16_t buttons;
};

uint16_t ButtonBit(std::string_view name) {
  constexpr std::pair<std::string_view, uint16_t> kButtons[] = {{"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010}, {"back", 0x0020}, {"lb", 0x0100}, {"rb", 0x0200}, {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
  for (const auto& [button, bit] : kButtons)
    if (button == name) return bit;
  return 0;
}

const std::vector<Press>& Script() {
  static const std::vector<Press> script = [] {
    std::vector<Press> presses;
    const std::string list = REXCVAR_GET(cherry_debug_press);
    for (size_t start = 0; start < list.size();) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) end = list.size();
      const std::string_view entry(list.data() + start, end - start);
      const size_t colon = entry.find(':');
      Press press{0, 0};
      if (colon != std::string_view::npos && std::from_chars(entry.data(), entry.data() + colon, press.frame).ec == std::errc()) {
        for (size_t b = colon + 1; b < entry.size();) {
          size_t plus = entry.find('+', b);
          if (plus == std::string_view::npos) plus = entry.size();
          press.buttons |= ButtonBit(entry.substr(b, plus - b));
          b = plus + 1;
        }
        if (press.buttons) presses.push_back(press);
      }
      start = end + 1;
    }
    return presses;
  }();
  return script;
}

}  // namespace

bool ScriptedPadActive() {
  return !Script().empty();
}

uint16_t ScriptedButtons() {
  const uint64_t frame = PresentedFrames();
  uint16_t buttons = 0;
  for (const Press& press : Script())
    if (frame >= press.frame && frame < press.frame + kHoldFrames) buttons |= press.buttons;
  return buttons;
}

}  // namespace cherry::engine::debug
