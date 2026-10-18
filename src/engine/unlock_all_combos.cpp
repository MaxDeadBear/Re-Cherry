#include "engine/unlock_all_combos.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/ue3_objects.h"

REXCVAR_DEFINE_BOOL(debug_unlock_all_combos, false, "Lollipop/Debug", "Every combo and every Nick ability is available while on; the save keeps what was really bought.");

namespace cherry::engine {

namespace {

// The game's own writers and savers of the flags run with the real values, so the save and the achievements only ever see those.
struct ScriptFunction {
  std::string_view cls;
  std::string_view name;
};

constexpr ScriptFunction kRealStateFunctions[] = {
    {"ASH_PlayerPowerUpLV", "InitDataLoad"},
    {"ASH_PlayerPowerUpLV", "InitUnlockTechnique"},
    {"ASH_PlayerPowerUpLV", "InitUnlockTechnique_verGC"},
    {"ASH_PlayerPowerUpLV", "FullUnlockTechnique"},
    {"ASH_PlayerPowerUpLV", "ShopUnlockTechFlag"},
    {"ASH_PlayerPowerUpLV", "AllComboOpenSearch"},
    {"ASH_SaveData", "LoadGameFromMemory_PlayerData"},
    {"ASH_SaveData", "SaveGame_PlayerData"},
    {"ASH_ShopItemDataList", "SetItemTableSoldOut"},
    {"ASH_SeqAct_StageStart", "Activated"},
};

constexpr uint64_t kRetryTicks = 60;

struct ForcedWord {
  uint32_t offset;
  uint32_t mask;
  uint32_t real;
};

struct State {
  bool resolved = false;
  uint32_t powerup_class = 0;
  std::vector<uint32_t> real_state_functions;
  std::vector<uint32_t> powerup_functions;
  std::vector<ForcedWord> words;
  uint32_t object = 0;
  bool forced = false;
  int depth = 0;
  uint64_t ticks = 0;
  uint64_t next_retry = 0;
};

State g;

void AddBits(uint32_t offset, uint32_t mask) {
  for (ForcedWord& word : g.words)
    if (word.offset == offset) {
      word.mask |= mask;
      return;
    }
  g.words.push_back({offset, mask, 0});
}

bool IsBool(uint32_t field) {
  return ue3::ClassCastFlags(ue3::ObjectClass(field)) & ue3::kCastBoolProperty;
}

bool Resolve() {
  const uint32_t cls = ue3::FindClass("ASH_PlayerPowerUpLV");
  const ue3::Property tech = cls ? ue3::FindProperty(cls, "mUnlockTechFlag") : ue3::Property{};
  if (!tech) return false;
  std::vector<uint32_t> functions;
  std::vector<uint32_t> powerup_functions;
  for (const ScriptFunction& f : kRealStateFunctions) {
    const uint32_t owner = ue3::FindClass(f.cls);
    const uint32_t function = owner ? ue3::FindField(owner, f.name) : 0;
    if (!function) return false;
    functions.push_back(function);
    if (owner == cls) powerup_functions.push_back(function);
  }
  g.words.clear();
  for (uint32_t field = ue3::FirstChild(tech.extra); field; field = ue3::NextField(field))
    if (IsBool(field)) AddBits(tech.offset + ue3::ReadProperty(field).offset, ue3::ReadProperty(field).extra);
  for (uint32_t field = ue3::FirstChild(cls); field; field = ue3::NextField(field))
    if (IsBool(field) && ue3::ObjectName(field).starts_with("mNick")) AddBits(ue3::ReadProperty(field).offset, ue3::ReadProperty(field).extra);
  g.powerup_class = cls;
  g.real_state_functions = std::move(functions);
  g.powerup_functions = std::move(powerup_functions);
  g.resolved = true;
  return true;
}

bool ObjectValid(uint32_t object) {
  return ue3::IsLive(object) && ue3::ObjectClass(object) == g.powerup_class;
}

void Force() {
  if (!g.forced) {
    std::string real;
    for (ForcedWord& word : g.words) {
      word.real = ue3::GetU32(g.object + word.offset) & word.mask;
      real += std::format(" +0x{:X}=0x{:08X}", word.offset, word.real);
    }
    REXLOG_INFO("[unlock_all_combos] real flags{}", real);
    g.forced = true;
  }
  for (const ForcedWord& word : g.words) {
    const uint32_t value = ue3::GetU32(g.object + word.offset);
    if ((value & word.mask) != word.mask) ue3::SetU32(g.object + word.offset, value | word.mask);
  }
}

void Release() {
  if (!g.forced) return;
  g.forced = false;
  if (!ObjectValid(g.object)) return;
  for (const ForcedWord& word : g.words) ue3::SetU32(g.object + word.offset, (ue3::GetU32(g.object + word.offset) & ~word.mask) | word.real);
}

bool FindObject() {
  if (ObjectValid(g.object)) return true;
  g.forced = false;
  const std::vector<uint32_t> instances = ue3::Instances(g.powerup_class);
  g.object = instances.empty() ? 0 : instances.front();
  if (g.object) REXLOG_INFO("[unlock_all_combos] forcing {} flag words on {}", g.words.size(), ue3::ObjectPath(g.object));
  return g.object != 0;
}

}  // namespace

void TickUnlockAllCombos() {
  ++g.ticks;
  if (!REXCVAR_GET(debug_unlock_all_combos)) {
    Release();
    return;
  }
  if (g.ticks < g.next_retry) return;
  if (!g.resolved && !Resolve()) {
    if (!g.next_retry) REXLOG_WARN("[unlock_all_combos] ASH_PlayerPowerUpLV or a script function it needs is not loaded yet");
    g.next_retry = g.ticks + kRetryTicks;
    return;
  }
  if (!FindObject()) {
    g.next_retry = g.ticks + kRetryTicks;
    return;
  }
  Force();
}

bool BeforeScriptCallCombos(uint32_t object, uint32_t function) {
  if (!g.resolved) return false;
  bool real_state = false;
  for (uint32_t f : g.real_state_functions) real_state |= f == function;
  if (!real_state) return false;
  bool own_function = false;
  for (uint32_t f : g.powerup_functions) own_function |= f == function;
  // Every map's ASH_GameInfo.PreBeginPlay builds a new ASH_IngameHUD and with it a new ASH_PlayerPowerUpLV, which its own InitDataLoad initialises; follow it there.
  if (own_function && object != g.object && ue3::ObjectClass(object) == g.powerup_class) {
    Release();
    g.object = object;
    REXLOG_INFO("[unlock_all_combos] forcing {} flag words on {}", g.words.size(), ue3::ObjectPath(object));
  }
  if (g.depth++ == 0) Release();
  return true;
}

void AfterScriptCallCombos() {
  if (--g.depth == 0 && REXCVAR_GET(debug_unlock_all_combos) && ObjectValid(g.object)) Force();
}

}  // namespace cherry::engine
