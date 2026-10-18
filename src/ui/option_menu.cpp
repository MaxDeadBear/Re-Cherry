#include "ui/option_menu.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
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

// The memory file FGFxFileOpener::OpenFile returns for a movie in a package: data at +8, length at +12, the url's first 62 characters at +20.
constexpr uint32_t kMemoryFileVtable = 0x8218A6E8;
// GFxMovieView::SetExternalInterfaceRetVal(const GFxValue&), the slot FGFxExternalInterface::Callback returns an UnrealScript result through.
constexpr uint32_t kSetExternalInterfaceRetValSlot = 200;
constexpr uint32_t kGFxValueBoolean = 2;
constexpr uint32_t kGFxValueNumber = 3;
constexpr std::string_view kControllerMarker = "ASHUS_SetOptionMenuVibMode";
constexpr std::string_view kCallPrefix = "CherryGfx_";

constexpr uint16_t kTagEnd = 0;
constexpr uint16_t kTagDoAction = 12;
constexpr uint16_t kTagDefineSprite = 39;

// The GRAPHICS rows in the order option_menu_graphics.as lists them, each with the cvar values its choices stand for.
struct Row {
  const char* cvar;
  bool boolean;
  std::array<const char*, 7> values;
};
constexpr Row kRows[] = {
    {"cherry_gfx_render_height", false, {"0", "144", "360", "720", "1080", "1440", "2160"}},
    {"cherry_gfx_fxaa", true, {"false", "true"}},
    {"cherry_gfx_anisotropy", false, {"0", "2", "4", "8", "16"}},
    {"cherry_gfx_stretch", true, {"false", "true"}},
    {"fullscreen", true, {"false", "true"}},
    {"cherry_gfx_vsync", true, {"false", "true"}},
    {"target_fps", false, {"30", "60", "0"}},
};
constexpr int kRowCount = int(std::size(kRows));

struct State {
  std::mutex mutex;
  std::filesystem::path config_path;
  std::function<void(std::function<void()>)> run_on_ui_thread;
  std::array<int, kRowCount> pending = {};
  std::vector<uint8_t> original_movie;
  uint32_t patched_address = 0;
  uint32_t patched_size = 0;
};

State& state() {
  static State s;
  return s;
}

int ChoiceCount(const Row& row) {
  return int(std::count_if(row.values.begin(), row.values.end(), [](const char* v) { return v != nullptr; }));
}

int IndexOf(const Row& row, const std::string& value) {
  if (row.boolean) return value == "true" || value == "1" ? 1 : 0;
  for (int i = 0; i < ChoiceCount(row); ++i) {
    if (std::atoi(row.values[i]) == std::atoi(value.c_str())) return i;
  }
  return 0;
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

// Inserts the GRAPHICS page's DoAction after the controller sprite's own, the DoAction that defines AllOptionSetting; empty when the movie is not the options screen.
std::vector<uint8_t> PatchMovie(const std::vector<uint8_t>& movie) {
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

void SwapOptionMenuMovie(uint8_t* base, uint32_t file) {
  const uint32_t data = REX_LOAD_U32(file + 8);
  const uint32_t size = REX_LOAD_U32(file + 12);
  const uint8_t* bytes = base + data;
  if (!data || size < 9 || std::memcmp(bytes, "GFX", 3) != 0) return;
  State& s = state();
  std::lock_guard lock(s.mutex);
  const bool known = s.patched_address && s.original_movie.size() == size && std::memcmp(s.original_movie.data(), bytes, size) == 0;
  if (!known) {
    if (!Contains(bytes, size, kControllerMarker)) return;
    std::vector<uint8_t> original(bytes, bytes + size);
    const std::vector<uint8_t> patched = PatchMovie(original);
    if (patched.empty()) {
      REXLOG_WARN("[ui] the options screen movie has no controller to add the GRAPHICS page to");
      return;
    }
    const uint32_t address = rex::system::kernel_state()->memory()->SystemHeapAlloc(uint32_t(patched.size()));
    if (!address) return;
    std::memcpy(base + address, patched.data(), patched.size());
    s.original_movie = std::move(original);
    s.patched_address = address;
    s.patched_size = uint32_t(patched.size());
    REXLOG_INFO("[ui] options screen movie patched with the GRAPHICS page ({} -> {} bytes)", size, s.patched_size);
  }
  REX_STORE_U32(file + 8, s.patched_address);
  REX_STORE_U32(file + 12, s.patched_size);
}

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
    for (int i = 0; i < kRowCount; ++i) {
      if (rex::cvar::GetFlagByName(kRows[i].cvar) != kRows[i].values[chosen[i]]) rex::cvar::SetFlagByName(kRows[i].cvar, kRows[i].values[chosen[i]]);
    }
    if (save) rex::cvar::SaveConfig(state().config_path);
  });
}

void HandleCall(PPCContext& ctx, uint8_t* base, std::string_view call) {
  const uint32_t movie = ctx.r4.u32;
  const uint32_t args = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;
  const int row = int(NumberArg(base, args, count, 0));
  const bool valid = row >= 0 && row < kRowCount;
  if (call == "Get" && valid) {
    ReturnNumber(ctx, base, movie, IndexOf(kRows[row], rex::cvar::GetFlagByName(kRows[row].cvar)));
  } else if (call == "Default" && valid) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(kRows[row].cvar);
    ReturnNumber(ctx, base, movie, info ? IndexOf(kRows[row], info->default_value) : 0);
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
      rex::cvar::SaveConfig(state().config_path);
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
  for (int i = 0; i < kRowCount; ++i) s.pending[i] = IndexOf(kRows[i], rex::cvar::GetFlagByName(kRows[i].cvar));
}

}  // namespace cherry::ui

// FGFxFileOpener::OpenFile(this, url)
REX_HOOK_RAW(sub_82932070) {
  __imp__sub_82932070(ctx, base);
  const uint32_t file = ctx.r3.u32;
  if (file && REX_LOAD_U32(file) == kMemoryFileVtable) SwapOptionMenuMovie(base, file);
}

// FGFxExternalInterface::Callback(this, movie_view, method_name, args, arg_count)
REX_HOOK_RAW(sub_829339B0) {
  const char* name = reinterpret_cast<const char*>(base + ctx.r5.u32);
  if (std::strncmp(name, kCallPrefix.data(), kCallPrefix.size()) != 0) {
    __imp__sub_829339B0(ctx, base);
    return;
  }
  HandleCall(ctx, base, name + kCallPrefix.size());
}
