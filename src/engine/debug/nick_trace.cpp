#include "engine/debug/nick_trace.h"

#include <bit>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/debug/script_dump.h"
#include "engine/ue3_objects.h"

REXCVAR_DEFINE_BOOL(cherry_debug_log_nick, false, "Lollipop/Debug", "Log every step of the Nick roulette, Chainsaw Dash and Chainsaw Blaster script path (input, gate checks, roulette slots, chosen move, Nick skill states) with arguments, results, tickets, Nick possession, the mNick* flags and the driver-chainsaw and weapon-load flags.");

namespace cherry::engine::debug {

namespace {

constexpr uint64_t kRetryTicks = 60;
constexpr int kMaxLoggedLocals = 6;

struct Traced {
  std::string_view cls;
  std::string_view path;
  bool snapshot;
  bool changes_only = false;
};

constexpr Traced kTraced[] = {
    {"ASH_PlayerController", "OnLeftThumbstick", true},
    {"ASH_PlayerController", "BeginNickRoulette", true},
    {"ASH_PlayerController", "NickRouletteAction", true},
    {"ASH_PlayerController", "NickRouletteCancel", false},
    {"ASH_PlayerController", "ChangeNickPossession", false},
    {"ASH_PlayerController", "OnButtonRB", true},
    {"ASH_GameInfo", "SetNickPossession", false},
    {"ASH_JulietPawn", "CheckNickTriggerStart", true},
    {"ASH_JulietPawn", "GetDriverChainsawFlag", false},
    {"ASH_JulietPawn", "SetDriverChainsawFlag", false},
    {"ASH_JulietPawn", "GotoStateNickIntro", false},
    {"ASH_JulietPawn", "GotoStateStarCassette", false},
    {"ASH_JulietPawn", "GotoStateNickShake", false},
    {"ASH_JulietPawn", "GotoStateAroundTheNick", false},
    {"ASH_JulietPawn", "GotoStateNickPoper", false},
    {"ASH_JulietPawn", "GotoStateNickShoot", false},
    {"ASH_JulietPawn", "SetWeaponDataLoadFlag", false},
    {"ASH_JulietPawn", "NickRouletteIntro.BeginState", false},
    {"ASH_JulietPawn", "NickRouletteIntro.OnAnimEnd", false},
    {"ASH_JulietPawn", "NickHangs.BeginState", true},
    {"ASH_JulietPawn", "NickShakeLoading.BeginState", true},
    {"ASH_JulietPawn", "NickShakeGalaxyStage.BeginState", true},
    {"ASH_GFxHUDNickRoulette", "ASHUS_CheckInitFrameNickReticle", true},
    {"ASH_GFxHUDNickRoulette", "CallAS_StartNickRoulette", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_GetNickRouletteItemUse", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_HitNickRouletteEnd", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_NickRouletteCancel", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_RouletteStopCall", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_NickRoulettePushStop", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_NickRouletteCancelCall", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_GetRouletteTime", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_GetRouletteGCVer", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_GetNickRouletteSpeed", false},
    {"ASH_GFxHUDNickRoulette", "ASHUS_CheckDebugCallBack", false},
    {"ASH_GFxHUDNickRoulette", "OnClose", false},
    {"ASH_JulietPawn", "TestRevolverStartStatus", true, true},
    {"ASH_JulietPawn", "ShotPoseIn.BeginState", false},
    {"ASH_SeqAct_RevolverWeapon", "Activated", false},
    {"ASH_GameInfo", "InitStageStart", false},
    {"ASH_PlayerController", "ClearRevolverStance", false},
    {"ASH_JulietPawn", "ChangePoseState", false},
    {"ASH_JulietPawn", "SetRevolverChainsawParam", false},
    {"ASH_WeaponManager", "SetCurrentMode", false},
    {"ASH_WeaponManager", "SetCurrentModeRef", false},
    {"ASH_WeaponModeRevolver", "SetMode", false},
    {"ASH_WeaponModeRevolver", "BeginRevolver", false},
    {"ASH_WeaponModeRevolver", "BeginCracker", false},
    {"ASH_IngameHUD", "DispAlertHUD", false},
    {"ASH_EnemyPawn", "DropAshItem", false},
    {"ASH_Item_Bullet", "Pickup", false},
    {"ASH_WeaponModeRevolver", "AddBulletNum", false},
    {"ASH_WeaponModeRevolver", "SetBulletNum", false},
    {"ASH_SeqAct_PermissionItemSpawn", "Activated", false},
};

// UObject::StateFrame (+0x18) and FStateFrame::StateNode (+0x28), as execGetStateName (0x8227C3F8) reads them.
constexpr uint32_t kObjectStateFrame = 0x18;
constexpr uint32_t kStateFrameNode = 0x28;

struct Function {
  uint32_t function;
  const Traced* traced;
  std::string last;
};

struct Call {
  Function* function;
  uint32_t object;
  uint32_t locals;
};

struct State {
  bool resolved = false;
  std::vector<Function> functions;
  std::vector<Call> stack;
  uint64_t ticks = 0;
  uint64_t next_retry = 0;
  uint32_t controller = 0;
  uint64_t next_controller_search = 0;
  std::string watched;
};

State g;

uint32_t FindPath(uint32_t cls, std::string_view path) {
  const size_t dot = path.find('.');
  if (dot == std::string_view::npos) return ue3::FindField(cls, path);
  const uint32_t state = ue3::FindField(cls, path.substr(0, dot));
  return state ? ue3::FindField(state, path.substr(dot + 1)) : 0;
}

bool Resolve() {
  if (!ue3::FindClass("ASH_JulietPawn")) return false;
  g.functions.clear();
  for (const Traced& t : kTraced) {
    const uint32_t cls = ue3::FindClass(t.cls);
    const uint32_t function = cls ? FindPath(cls, t.path) : 0;
    if (function) g.functions.push_back({function, &t});
    else REXLOG_WARN("[nick] {}.{} not found, not traced", t.cls, t.path);
  }
  return true;
}

bool IsProperty(uint32_t field) {
  return ue3::ClassCastFlags(ue3::ObjectClass(field)) & ue3::kCastProperty;
}

std::string Value(uint32_t object, uint32_t cls, std::string_view name) {
  const ue3::Property p = cls ? ue3::FindProperty(cls, name) : ue3::Property{};
  return p ? FormatScriptValue(object + p.offset, p.object) : "?";
}

std::string Values(uint32_t object, std::initializer_list<std::string_view> names) {
  const uint32_t cls = ue3::ObjectClass(object);
  std::string text;
  for (std::string_view name : names) text += std::format(" {}={}", name, Value(object, cls, name));
  return text;
}

std::string NickFlags(uint32_t list) {
  std::string text;
  for (uint32_t field = ue3::FirstChild(ue3::ObjectClass(list)); field; field = ue3::NextField(field))
    if (IsProperty(field) && ue3::ObjectName(field).starts_with("mNick")) text += std::format(" {}={}", ue3::ObjectName(field), FormatScriptValue(list + ue3::ReadProperty(field).offset, field));
  return text;
}

std::vector<uint32_t> Live(std::string_view cls_name) {
  const uint32_t cls = ue3::FindClass(cls_name);
  return cls ? ue3::Instances(cls) : std::vector<uint32_t>{};
}

void LogRemoteEventHandlers(std::string_view what, uint32_t name_address) {
  const uint32_t index = ue3::GetU32(name_address);
  const uint32_t number = ue3::GetU32(name_address + 4);
  const uint32_t cls = ue3::FindClass("SeqEvent_RemoteEvent");
  const ue3::Property event_name = cls ? ue3::FindProperty(cls, "EventName") : ue3::Property{};
  int count = 0;
  if (event_name)
    for (uint32_t event : ue3::Instances(cls))
      if (ue3::GetU32(event + event_name.offset) == index && ue3::GetU32(event + event_name.offset + 4) == number) {
        if (count++ < 4) REXLOG_INFO("[nick]   handler {}{}", ue3::ObjectPath(event), Values(event, {"bEnabled"}));
      }
  REXLOG_INFO("[nick]   {} '{}': {} SeqEvent_RemoteEvent handler(s) loaded", what, ue3::NameString(index, number), count);
}

void LogSnapshot(uint32_t object) {
  for (uint32_t controller : Live("ASH_PlayerController")) {
    REXLOG_INFO("[nick]   controller {}{}", ue3::ObjectPath(controller), Values(controller, {"bNickPossession", "bActionUse", "mNickRouletteActive", "mNickRouletteActionId", "mIsEnableRevolverWeapon", "Heat"}));
    const uint32_t cls = ue3::ObjectClass(controller);
    const ue3::Property juliet = ue3::FindProperty(cls, "Juliet");
    const uint32_t pawn = juliet ? ue3::GetU32(controller + juliet.offset) : 0;
    if (pawn) REXLOG_INFO("[nick]   juliet {}{}", ue3::ObjectPath(pawn), Values(pawn, {"NickRouletteCount", "mDriverChainsawFlag", "mbWeaponLoadFlag", "NickShakeType", "mbModeNickZombie", "mIsToggleCinematicMode", "mUniqueActionCoolTime", "bTurkey", "bPendingTurkeyStart", "mPoseState"}));
  }
  for (uint32_t hud : Live("ASH_IngameHUD")) REXLOG_INFO("[nick]   hud {}{}", ue3::ObjectPath(hud), Values(hud, {"PlayerFlgMng", "bLockflg"}));
  for (uint32_t list : Live("ASH_PlayerPowerUpLV")) REXLOG_INFO("[nick]   flags {}{}", ue3::ObjectPath(list), NickFlags(list));
  const uint32_t cls = ue3::ObjectClass(object);
  const ue3::Property load = ue3::FindProperty(cls, "NickShakeLoadEventName");
  if (load) LogRemoteEventHandlers("NickShakeLoadEventName", object + load.offset);
  const ue3::Property warp = ue3::FindProperty(cls, "NickShakeWarpEventName");
  if (warp) LogRemoteEventHandlers("NickShakeWarpEventName", object + warp.offset);
}

std::string Arguments(uint32_t function, uint32_t locals) {
  std::string text;
  int logged = 0;
  for (uint32_t field = ue3::FirstChild(function); field && logged < kMaxLoggedLocals; field = ue3::NextField(field)) {
    if (!IsProperty(field) || ue3::ObjectName(field) == "ReturnValue") continue;
    text += std::format(" {}={}", ue3::ObjectName(field), FormatScriptValue(locals + ue3::ReadProperty(field).offset, field));
    ++logged;
  }
  return text;
}

std::string StateName(uint32_t object) {
  const uint32_t frame = ue3::GetU32(object + kObjectStateFrame);
  const uint32_t node = frame ? ue3::GetU32(frame + kStateFrameNode) : 0;
  return node ? ue3::ObjectName(node) : "None";
}

uint32_t ObjectField(uint32_t object, std::string_view name) {
  const ue3::Property p = object ? ue3::FindProperty(ue3::ObjectClass(object), name) : ue3::Property{};
  return p ? ue3::GetU32(object + p.offset) : 0;
}

// Logs, once per change, the player state the Blaster and Nick paths depend on, including writes no traced script function makes (level script, native code).
void WatchPlayer() {
  if (!ue3::IsLive(g.controller) && g.ticks >= g.next_controller_search) {
    const std::vector<uint32_t> controllers = Live("ASH_PlayerController");
    g.controller = controllers.empty() ? 0 : controllers.front();
    g.next_controller_search = g.ticks + kRetryTicks;
  }
  if (!ue3::IsLive(g.controller)) return;
  const uint32_t pawn = ObjectField(g.controller, "Juliet");
  const uint32_t input = ObjectField(g.controller, "PlayerInput");
  const uint32_t manager = pawn ? ObjectField(pawn, "WeaponManager") : 0;
  const uint32_t revolver = manager ? ObjectField(manager, "WeaponRevolver") : 0;
  std::string text = std::format(" controller={}{}", StateName(g.controller), Values(g.controller, {"bLTButtonPressed", "mIsEnableRevolverWeapon"}));
  const ue3::Property trigger = input ? ue3::FindProperty(ue3::ObjectClass(input), "aLeftTrigger") : ue3::Property{};
  if (trigger) text += std::format(" LT>0.3={}", std::bit_cast<float>(ue3::GetU32(input + trigger.offset)) > 0.3f);
  if (pawn) text += std::format(" juliet={}{}", StateName(pawn), Values(pawn, {"mPoseState"}));
  if (manager) text += Values(manager, {"CurrentMode"});
  if (revolver) text += Values(revolver, {"NowRevolverMode", "mbAimRevolver", "mShotBulletNum"});
  if (text == g.watched) return;
  g.watched = text;
  REXLOG_INFO("[nick] watch{}", text);
}

}  // namespace

void TickNickTrace() {
  ++g.ticks;
  if (!REXCVAR_GET(cherry_debug_log_nick)) return;
  if (g.resolved) {
    WatchPlayer();
    return;
  }
  if (g.ticks < g.next_retry) return;
  g.resolved = Resolve();
  if (g.resolved) REXLOG_INFO("[nick] tracing {} script functions", g.functions.size());
  else g.next_retry = g.ticks + kRetryTicks;
}

bool BeforeScriptCallNickTrace(uint32_t object, uint32_t function, uint32_t locals) {
  if (!g.resolved || !REXCVAR_GET(cherry_debug_log_nick)) return false;
  for (Function& f : g.functions) {
    if (f.function != function) continue;
    if (!f.traced->changes_only) {
      REXLOG_INFO("[nick] > {}.{} on {}:{}", f.traced->cls, f.traced->path, ue3::ObjectPath(object), Arguments(function, locals));
      if (f.traced->snapshot) LogSnapshot(object);
    }
    g.stack.push_back({&f, object, locals});
    return true;
  }
  return false;
}

void AfterScriptCallNickTrace(uint32_t result) {
  const Call call = g.stack.back();
  g.stack.pop_back();
  const ue3::Property ret = ue3::FindProperty(call.function->function, "ReturnValue");
  std::string value = "-";
  // A returned bool is the u32 0 or 1 that execTrue (0x8226B4D0) and execBoolVariable (0x82269480) store, not the property's bit mask.
  if (ret && result) value = (ue3::ClassCastFlags(ue3::ObjectClass(ret.object)) & ue3::kCastBoolProperty) ? std::format("{} (0x{:X})", ue3::GetU32(result) ? "true" : "false", ue3::GetU32(result)) : FormatScriptValue(result, ret.object);
  // Checks the game repeats every frame while a button is held are logged only when their answer changes.
  if (call.function->traced->changes_only) {
    if (value == call.function->last) return;
    call.function->last = value;
    REXLOG_INFO("[nick] = {}.{} on {} now returns {}", call.function->traced->cls, call.function->traced->path, ue3::ObjectPath(call.object), value);
    if (call.function->traced->snapshot) LogSnapshot(call.object);
    return;
  }
  REXLOG_INFO("[nick] < {}.{} = {} | locals:{}", call.function->traced->cls, call.function->traced->path, value, Arguments(call.function->function, call.locals));
  if (call.function->traced->path == "DropAshItem") {
    REXLOG_INFO("[nick]   drop from {}{}", ue3::ObjectPath(call.object), Values(call.object, {"bEnableDropItem", "ItemList", "MedalCount"}));
    const uint32_t game_info = ObjectField(call.object, "WorldInfo") ? ObjectField(ObjectField(call.object, "WorldInfo"), "Game") : 0;
    if (game_info) REXLOG_INFO("[nick]   {}{}", ue3::ObjectPath(game_info), Values(game_info, {"bPermissionItemSpawn"}));
  }
  if (call.function->traced->path == "ShotPoseIn.BeginState") REXLOG_INFO("[nick]   blaster unstow anim time (0 plus the aim adjust means the animation is missing){}", Values(call.object, {"mShotInAimTime", "fCofig_Juliet_AutoAimAdjustTime"}));
  if (call.function->traced->path == "ASHUS_GetNickRouletteItemUse") {
    REXLOG_INFO("[nick]   roulette {}{}", ue3::ObjectPath(call.object), Values(call.object, {"bNickRouletteUse", "mNRDeadFlg"}));
    const ue3::Property list = ue3::FindProperty(call.function->function, "lPlayerFlgList");
    const uint32_t flags = list ? ue3::GetU32(call.locals + list.offset) : 0;
    if (flags) REXLOG_INFO("[nick]   read from {}{}", ue3::ObjectPath(flags), NickFlags(flags));
  }
}

}  // namespace cherry::engine::debug
