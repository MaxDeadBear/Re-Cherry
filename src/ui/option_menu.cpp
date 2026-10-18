#include "ui/option_menu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "ui/actionscript/actionscript.h"

namespace {

// GFxMovieView::SetExternalInterfaceRetVal(const GFxValue&), the slot FGFxExternalInterface::Callback returns an UnrealScript result through.
constexpr uint32_t kSetExternalInterfaceRetValSlot = 200;
constexpr uint32_t kGFxValueBoolean = 2;
constexpr uint32_t kGFxValueNumber = 3;
constexpr std::string_view kControllerMarker = "ASHUS_SetOptionMenuVibMode";
constexpr std::string_view kCallPrefix = "CherryGfx_";

constexpr uint16_t kTagEnd = 0;
constexpr uint16_t kTagDoAction = 12;
constexpr uint16_t kTagDefineSprite = 39;

// The GRAPHICS rows in the order option_menu_graphics.as lists them, each with the cvar values its choices stand for; a row may set a second cvar with each choice too.
struct Row {
  const char* cvar;
  std::array<const char*, 7> values;
  const char* cvar2 = nullptr;
  std::array<const char*, 7> values2 = {};
};
constexpr Row kRows[] = {
    {"cherry_gfx_render_height", {"0", "144", "360", "720", "1080", "1440", "2160"}},
    {"cherry_gfx_fxaa", {"false", "true"}},
    {"cherry_gfx_anisotropy", {"0", "2", "4", "8", "16"}},
    {"cherry_gfx_stretch", {"false", "true", "false", "false"}, "cherry_gfx_ultrawide", {"0", "0", "21", "32"}},
    {"fullscreen", {"false", "true"}},
    {"cherry_gfx_vsync", {"false", "true"}},
    {"target_fps", {"30", "60", "0"}},
};
constexpr int kRowCount = int(std::size(kRows));

struct State {
  std::mutex mutex;
  std::filesystem::path config_path;
  std::function<void(std::function<void()>)> run_on_ui_thread;
  std::array<int, kRowCount> pending = {};
};

State& state() {
  static State s;
  return s;
}

int ChoiceCount(const Row& row) {
  return int(std::count_if(row.values.begin(), row.values.end(), [](const char* v) { return v != nullptr; }));
}

// Whether a cvar's value is this choice's, comparing booleans and numbers by meaning rather than spelling.
bool Matches(const char* cvar, const std::string& value, const char* choice) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(cvar);
  if (info && info->type == rex::cvar::FlagType::Boolean) return (value == "true" || value == "1") == (std::string_view(choice) == "true");
  return std::atoi(value.c_str()) == std::atoi(choice);
}

// The first choice both of whose cvars hold its values, from `value_of` (the live value or the default); 0 when none does.
template <typename ValueOf>
int ChoiceOf(const Row& row, ValueOf value_of) {
  for (int i = 0; i < ChoiceCount(row); ++i) {
    if (Matches(row.cvar, value_of(row.cvar), row.values[i]) && (!row.cvar2 || Matches(row.cvar2, value_of(row.cvar2), row.values2[i]))) return i;
  }
  return 0;
}

int CurrentChoice(const Row& row) {
  return ChoiceOf(row, [](const char* cvar) { return rex::cvar::GetFlagByName(cvar); });
}

int DefaultChoice(const Row& row) {
  return ChoiceOf(row, [](const char* cvar) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(cvar);
    return info ? info->default_value : std::string();
  });
}

struct TagHeader {
  uint16_t code;
  uint32_t length;
  uint32_t header_size;
};

bool ReadTag(const std::vector<uint8_t>& data, size_t pos, TagHeader& tag) {
  if (pos + 2 > data.size()) return false;
  const uint16_t word = uint16_t(data[pos] | (data[pos + 1] << 8));
  tag.code = word >> 6;
  tag.length = word & 0x3F;
  tag.header_size = 2;
  if (tag.length == 0x3F) {
    if (pos + 6 > data.size()) return false;
    tag.length = uint32_t(data[pos + 2]) | (uint32_t(data[pos + 3]) << 8) | (uint32_t(data[pos + 4]) << 16) | (uint32_t(data[pos + 5]) << 24);
    tag.header_size = 6;
  }
  return pos + tag.header_size + tag.length <= data.size();
}

void WriteU32(std::vector<uint8_t>& data, size_t pos, uint32_t value) {
  for (int i = 0; i < 4; ++i) data[pos + i] = uint8_t(value >> (8 * i));
}

bool Contains(const uint8_t* data, size_t size, std::string_view text) {
  return std::search(data, data + size, text.begin(), text.end()) != data + size;
}

}  // namespace

namespace cherry::ui {

std::vector<uint8_t> AddGraphicsPage(const std::vector<uint8_t>& movie) {
  if (movie.size() < 9 || std::memcmp(movie.data(), "GFX", 3) != 0) return {};
  const uint32_t rect_bits = 5 + 4 * (movie[8] >> 3);
  size_t pos = 8 + (rect_bits + 7) / 8 + 4;
  TagHeader tag;
  while (ReadTag(movie, pos, tag) && tag.code != kTagEnd) {
    const size_t body = pos + tag.header_size;
    if (tag.code == kTagDefineSprite && tag.header_size == 6 && Contains(movie.data() + body, tag.length, kControllerMarker)) {
      size_t insert_at = 0;
      TagHeader sub;
      for (size_t sub_pos = body + 4; sub_pos < body + tag.length && ReadTag(movie, sub_pos, sub) && sub.code != kTagEnd; sub_pos += sub.header_size + sub.length) {
        if (sub.code == kTagDoAction && Contains(movie.data() + sub_pos + sub.header_size, sub.length, kControllerMarker)) insert_at = sub_pos + sub.header_size + sub.length;
      }
      if (!insert_at) return {};
      const auto& code = cherry::ui::actionscript::g_optionMenuGraphics;
      std::vector<uint8_t> inserted(6 + sizeof(code));
      inserted[0] = uint8_t((kTagDoAction << 6) | 0x3F);
      inserted[1] = uint8_t(kTagDoAction >> 2);
      WriteU32(inserted, 2, uint32_t(sizeof(code)));
      std::memcpy(inserted.data() + 6, code, sizeof(code));
      std::vector<uint8_t> patched = movie;
      patched.insert(patched.begin() + insert_at, inserted.begin(), inserted.end());
      WriteU32(patched, pos + 2, tag.length + uint32_t(inserted.size()));
      WriteU32(patched, 4, uint32_t(patched.size()));
      return patched;
    }
    pos = body + tag.length;
  }
  return {};
}

}  // namespace cherry::ui

namespace {

double NumberArg(uint8_t* base, uint32_t args, uint32_t count, uint32_t index) {
  if (index >= count) return 0.0;
  const uint32_t value = args + 16 * index;
  const uint32_t type = REX_LOAD_U32(value + 4) & 0x8F;
  if (type == kGFxValueNumber) return std::bit_cast<double>(uint64_t(REX_LOAD_U64(value + 8)));
  if (type == kGFxValueBoolean) return REX_LOAD_U8(value + 8) ? 1.0 : 0.0;
  return 0.0;
}

// Builds a GFxValue below the hooked call's stack pointer and hands it to the movie as the call's result.
void ReturnNumber(PPCContext& ctx, uint8_t* base, uint32_t movie, double number) {
  const uint32_t sp = ctx.r1.u32;
  const uint32_t value = (sp - 0x100) & ~0xFu;
  REX_STORE_U32(value, 0);
  REX_STORE_U32(value + 4, kGFxValueNumber);
  REX_STORE_U64(value + 8, std::bit_cast<uint64_t>(number));
  ctx.r1.u32 = value - 0x100;
  ctx.r3.u32 = movie;
  ctx.r4.u32 = value;
  rex::runtime::ResolveIndirectFunction(REX_LOAD_U32(REX_LOAD_U32(movie) + kSetExternalInterfaceRetValSlot))(ctx, base);
  ctx.r1.u32 = sp;
}

// Writes just these cvars into the config file, keeping its other lines; the SDK's SaveConfig would also persist whatever the command line set, debug flags included.
void SaveCvars(const std::filesystem::path& path, const std::vector<std::string_view>& names) {
  std::vector<std::string> lines;
  if (std::ifstream in(path); in) {
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  if (lines.empty()) lines.push_back("# Auto-generated cvar configuration");
  for (std::string_view name : names) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
    if (!info) continue;
    std::string value = rex::cvar::GetFlagByName(name);
    if (info->type == rex::cvar::FlagType::String) value = "\"" + value + "\"";
    const std::string entry = std::string(name) + " = " + value;
    auto match = std::find_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.starts_with(std::string(name) + " ") || line.starts_with(std::string(name) + "="); });
    if (match != lines.end()) *match = entry;
    else lines.push_back(entry);
  }
  std::ofstream out(path, std::ios::trunc);
  for (const std::string& line : lines) out << line << '\n';
}

// Sets the staged choices on the UI thread, as the settings overlay does, so they take effect at once; `save` also writes the config.
void ApplyGraphics(bool save) {
  State& s = state();
  std::array<int, kRowCount> chosen;
  {
    std::lock_guard lock(s.mutex);
    chosen = s.pending;
    if (!s.run_on_ui_thread) return;
  }
  s.run_on_ui_thread([chosen, save] {
    std::vector<std::string_view> names;
    auto set = [&](const char* cvar, const char* value) {
      if (!Matches(cvar, rex::cvar::GetFlagByName(cvar), value)) rex::cvar::SetFlagByName(cvar, value);
      names.push_back(cvar);
    };
    for (int i = 0; i < kRowCount; ++i) {
      set(kRows[i].cvar, kRows[i].values[chosen[i]]);
      if (kRows[i].cvar2) set(kRows[i].cvar2, kRows[i].values2[chosen[i]]);
    }
    if (save) SaveCvars(state().config_path, names);
  });
}

void HandleCall(PPCContext& ctx, uint8_t* base, std::string_view call) {
  const uint32_t movie = ctx.r4.u32;
  const uint32_t args = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;
  const int row = int(NumberArg(base, args, count, 0));
  const bool valid = row >= 0 && row < kRowCount;
  if (call == "Get" && valid) {
    ReturnNumber(ctx, base, movie, CurrentChoice(kRows[row]));
  } else if (call == "Default" && valid) {
    ReturnNumber(ctx, base, movie, DefaultChoice(kRows[row]));
  } else if (call == "Set" && valid) {
    const int choice = int(NumberArg(base, args, count, 1));
    {
      std::lock_guard lock(state().mutex);
      state().pending[row] = std::clamp(choice, 0, ChoiceCount(kRows[row]) - 1);
    }
    ApplyGraphics(false);
  } else if (call == "Save") {
    ApplyGraphics(true);
  } else if (call == "ToggleFps") {
    std::lock_guard lock(state().mutex);
    if (state().run_on_ui_thread) state().run_on_ui_thread([] {
      rex::cvar::SetFlagByName("cherry_fps_counter", rex::cvar::Query<bool>("cherry_fps_counter") ? "false" : "true");
      SaveCvars(state().config_path, {std::string_view("cherry_fps_counter")});
    });
  }
}

}  // namespace

namespace cherry::ui {

void InitOptionMenu(const std::filesystem::path& config_path, std::function<void(std::function<void()>)> run_on_ui_thread) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  s.config_path = config_path;
  s.run_on_ui_thread = std::move(run_on_ui_thread);
  for (int i = 0; i < kRowCount; ++i) s.pending[i] = CurrentChoice(kRows[i]);
}

}  // namespace cherry::ui

// FGFxExternalInterface::Callback(this, movie_view, method_name, args, arg_count)
REX_HOOK_RAW(sub_829339B0) {
  const char* name = reinterpret_cast<const char*>(base + ctx.r5.u32);
  if (std::strncmp(name, kCallPrefix.data(), kCallPrefix.size()) != 0) {
    __imp__sub_829339B0(ctx, base);
    return;
  }
  HandleCall(ctx, base, name + kCallPrefix.size());
}
