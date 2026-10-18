#include "engine/unlock_all_moves.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "engine/ue3_objects.h"

REXCVAR_DEFINE_BOOL(debug_unlock_all_moves, false, "Lollipop/Debug", "Chainsaw Dash, the Chainsaw Blaster and the Nick roulette work in every level, even where the story has not yet given Juliet the dash, the gun, Nick or a roulette ticket; nothing is saved.");

namespace cherry::engine {

namespace {

constexpr uint64_t kRetryTicks = 60;
constexpr uint8_t kModeChainsaw = 0;
constexpr uint8_t kModeBlaster = 1;
constexpr uint8_t kPoseAim = 3;
constexpr uint32_t kPermissionOtherItem = 3;

enum class Check { DriverChainsaw, NickTrigger, NickRoulette, Blaster, PlayerTick, EnemyDrop };

// What a check was lent for its duration and gets back afterwards.
struct Lent {
  Check check;
  uint32_t pawn = 0;
  uint32_t count = 0;
  bool count_lent = false;
  uint32_t controller = 0;
  bool possession_lent = false;
  bool blaster_lent = false;
  uint32_t weapon_manager = 0;
  uint32_t locals = 0;
  bool drop_enabled = false;
};

struct State {
  bool resolved = false;
  uint32_t get_driver_chainsaw = 0;
  uint32_t check_nick_trigger = 0;
  uint32_t begin_nick_roulette = 0;
  uint32_t test_revolver_start = 0;
  uint32_t player_tick = 0;
  ue3::Property roulette_count;
  ue3::Property possession;
  ue3::Property controller_juliet;
  ue3::Property juliet_controller;
  ue3::Property revolver_enabled;
  ue3::Property pose_state;
  ue3::Property weapon_manager;
  ue3::Property current_mode;
  ue3::Property player_input;
  ue3::Property left_trigger;
  // Blaster ammo for enemy drops, resolved separately so a missing piece only disables the ammo.
  bool ammo_resolved = false;
  uint32_t drop_ash_item = 0;
  ue3::Property enable_drop;
  ue3::Property drop_game_info;
  ue3::Property permission_item_spawn;
  ue3::Property weapon_revolver;
  ue3::Property bullet_num;
  ue3::Property ammo_max;
  uint32_t bullet_archetype = 0;
  uint32_t bullet_amount = 0;
  uint32_t player_pawn = 0;
  // The WeaponManager whose CurrentMode was raised from chainsaw (0) to the Blaster weapon (1) for an aim, until the aim ends.
  uint32_t mode_forced_on = 0;
  std::vector<Lent> stack;
  uint64_t ticks = 0;
  uint64_t next_retry = 0;
};

State g;

bool Resolve() {
  const uint32_t pawn = ue3::FindClass("ASH_JulietPawn");
  const uint32_t controller = ue3::FindClass("ASH_PlayerController");
  if (!pawn || !controller) return false;
  g.get_driver_chainsaw = ue3::FindField(pawn, "GetDriverChainsawFlag");
  g.check_nick_trigger = ue3::FindField(pawn, "CheckNickTriggerStart");
  g.begin_nick_roulette = ue3::FindField(controller, "BeginNickRoulette");
  g.roulette_count = ue3::FindProperty(pawn, "NickRouletteCount");
  g.possession = ue3::FindProperty(controller, "bNickPossession");
  g.controller_juliet = ue3::FindProperty(controller, "Juliet");
  g.test_revolver_start = ue3::FindField(pawn, "TestRevolverStartStatus");
  g.juliet_controller = ue3::FindProperty(pawn, "mJulietController");
  g.revolver_enabled = ue3::FindProperty(controller, "mIsEnableRevolverWeapon");
  g.player_tick = ue3::FindField(controller, "PlayerTick");
  g.pose_state = ue3::FindProperty(pawn, "mPoseState");
  g.weapon_manager = ue3::FindProperty(pawn, "WeaponManager");
  const uint32_t manager = ue3::FindClass("ASH_WeaponManager");
  g.current_mode = manager ? ue3::FindProperty(manager, "CurrentMode") : ue3::Property{};
  g.player_input = ue3::FindProperty(controller, "PlayerInput");
  const uint32_t input = ue3::FindClass("ASH_PlayerInput");
  g.left_trigger = input ? ue3::FindProperty(input, "aLeftTrigger") : ue3::Property{};
  return g.get_driver_chainsaw && g.check_nick_trigger && g.begin_nick_roulette && g.roulette_count && g.possession && g.controller_juliet && g.test_revolver_start && g.juliet_controller && g.revolver_enabled && g.player_tick && g.pose_state && g.weapon_manager && g.current_mode && g.player_input && g.left_trigger;
}

// The ASH_Item_Bullet archetype IT_Bullet.Archetypes.AR_BulletS_Cracker_Drop, which IT_Bullet keeps loaded from the title on; its amount is what ASH_Item_Bullet.Pickup adds.
uint32_t FindBulletArchetype() {
  const uint32_t cls = ue3::FindClass("ASH_Item_Bullet");
  if (!cls) return 0;
  for (uint32_t i = 0, count = ue3::ObjectCount(); i < count; ++i) {
    const uint32_t object = ue3::ObjectAt(i);
    if (object && ue3::ObjectClass(object) == cls && ue3::ObjectName(object) == "AR_BulletS_Cracker_Drop") return object;
  }
  return 0;
}

uint32_t PickupAmount(uint32_t archetype) {
  static constexpr std::string_view kCounts[] = {"Bullet_AmountTypeS_Count", "Bullet_AmountTypeM_Count", "Bullet_AmountTypeL_Count"};
  const uint32_t cls = ue3::ObjectClass(archetype);
  const ue3::Property type = ue3::FindProperty(cls, "BulletAmountType");
  const uint8_t index = type ? ue3::GetU8(archetype + type.offset) : 0;
  const ue3::Property count = index < 3 ? ue3::FindProperty(cls, kCounts[index]) : ue3::Property{};
  return count ? ue3::GetU32(archetype + count.offset) : 0;
}

bool ResolveAmmo() {
  const uint32_t enemy = ue3::FindClass("ASH_EnemyPawn");
  const uint32_t game_info = ue3::FindClass("ASH_GameInfo");
  const uint32_t manager = ue3::FindClass("ASH_WeaponManager");
  const uint32_t revolver = ue3::FindClass("ASH_WeaponModeRevolver");
  const uint32_t pawn = ue3::FindClass("ASH_JulietPawn");
  if (!enemy || !game_info || !manager || !revolver || !pawn) return false;
  g.drop_ash_item = ue3::FindField(enemy, "DropAshItem");
  g.enable_drop = ue3::FindProperty(enemy, "bEnableDropItem");
  g.drop_game_info = g.drop_ash_item ? ue3::FindProperty(g.drop_ash_item, "lGameInfo") : ue3::Property{};
  g.permission_item_spawn = ue3::FindProperty(game_info, "bPermissionItemSpawn");
  g.weapon_revolver = ue3::FindProperty(manager, "WeaponRevolver");
  g.bullet_num = ue3::FindProperty(revolver, "mShotBulletNum");
  g.ammo_max = ue3::FindProperty(pawn, "fConfig_Juliet_Ammo_Max");
  g.bullet_archetype = FindBulletArchetype();
  g.bullet_amount = g.bullet_archetype ? PickupAmount(g.bullet_archetype) : 0;
  return g.drop_ash_item && g.enable_drop && g.drop_game_info && g.permission_item_spawn && g.weapon_revolver && g.bullet_num && g.ammo_max && g.bullet_amount;
}

// IsPermissionItemSpawn(permission): bPermissionItemSpawn is a dynamic array of bools.
bool ItemSpawnPermitted(uint32_t game_info, uint32_t permission) {
  const uint32_t data = ue3::GetU32(game_info + g.permission_item_spawn.offset);
  if (!data || permission >= ue3::GetU32(game_info + g.permission_item_spawn.offset + 4)) return false;
  const ue3::Property inner = ue3::ReadProperty(g.permission_item_spawn.extra);
  return ue3::GetU32(data + permission * inner.element_size) & inner.extra;
}

// The stage has not granted the Blaster while its own weapon mode is still the chainsaw; aiming there borrows mode 1 (see PlayerTick below).
bool BeforeGrant(uint32_t pawn) {
  const uint32_t manager = ue3::GetU32(pawn + g.weapon_manager.offset);
  return manager && (ue3::GetU8(manager + g.current_mode.offset) == kModeChainsaw || g.mode_forced_on == manager);
}

// What ASH_Item_Bullet.Pickup does through AddBulletNum: add the pickup's amount, capped at fConfig_Juliet_Ammo_Max.
void AddBlasterAmmo(uint32_t pawn) {
  const uint32_t manager = ue3::GetU32(pawn + g.weapon_manager.offset);
  const uint32_t revolver = manager ? ue3::GetU32(manager + g.weapon_revolver.offset) : 0;
  if (!revolver) return;
  const int32_t max = int32_t(ue3::GetU32(pawn + g.ammo_max.offset));
  const int32_t bullets = std::min(int32_t(ue3::GetU32(revolver + g.bullet_num.offset)) + int32_t(g.bullet_amount), max);
  ue3::SetU32(revolver + g.bullet_num.offset, uint32_t(bullets));
}

void LendTicket(Lent& lent, uint32_t pawn) {
  if (!pawn) return;
  lent.pawn = pawn;
  lent.count = ue3::GetU32(pawn + g.roulette_count.offset);
  if (int32_t(lent.count) > 0) return;
  ue3::SetU32(pawn + g.roulette_count.offset, 1);
  lent.count_lent = true;
}

}  // namespace

void TickUnlockAllMoves() {
  ++g.ticks;
  if (g.resolved || !REXCVAR_GET(debug_unlock_all_moves) || g.ticks < g.next_retry) return;
  g.resolved = Resolve();
  if (!g.resolved) {
    g.next_retry = g.ticks + kRetryTicks;
    return;
  }
  REXLOG_INFO("[unlock_all_moves] Chainsaw Dash, Chainsaw Blaster and Nick roulette checks found");
  g.ammo_resolved = ResolveAmmo();
  if (g.ammo_resolved) REXLOG_INFO("[unlock_all_moves] enemies drop {} Blaster ammo ({}) before the Blaster is granted", g.bullet_amount, ue3::ObjectPath(g.bullet_archetype));
  else REXLOG_WARN("[unlock_all_moves] Blaster ammo drops unavailable: a drop property or IT_Bullet.Archetypes.AR_BulletS_Cracker_Drop is not loaded");
}

bool BeforeScriptCallMoves(uint32_t object, uint32_t function, uint32_t locals) {
  if (!g.resolved || !REXCVAR_GET(debug_unlock_all_moves)) return false;
  Lent lent{Check::DriverChainsaw};
  if (function == g.check_nick_trigger) {
    lent.check = Check::NickTrigger;
    LendTicket(lent, object);
  } else if (function == g.begin_nick_roulette) {
    lent.check = Check::NickRoulette;
    lent.controller = object;
    if (!ue3::GetBool(object, g.possession)) {
      ue3::SetBool(object, g.possession, true);
      lent.possession_lent = true;
    }
    LendTicket(lent, ue3::GetU32(object + g.controller_juliet.offset));
  } else if (function == g.test_revolver_start) {
    lent.check = Check::Blaster;
    lent.controller = ue3::GetU32(object + g.juliet_controller.offset);
    if (lent.controller && !ue3::GetBool(lent.controller, g.revolver_enabled)) {
      ue3::SetBool(lent.controller, g.revolver_enabled, true);
      lent.blaster_lent = true;
    }
  } else if (function == g.player_tick) {
    // ASH_PlayerController.PlayerTick handles LT (aim) only while WeaponManager.CurrentMode is 1; give it 1 while LT is held or the aim pose is up.
    lent.check = Check::PlayerTick;
    lent.pawn = ue3::GetU32(object + g.controller_juliet.offset);
    g.player_pawn = lent.pawn;
    const uint32_t input = ue3::GetU32(object + g.player_input.offset);
    lent.weapon_manager = lent.pawn ? ue3::GetU32(lent.pawn + g.weapon_manager.offset) : 0;
    if (!lent.weapon_manager || !input) return false;
    const bool aiming = std::bit_cast<float>(ue3::GetU32(input + g.left_trigger.offset)) > 0.3f || ue3::GetU8(lent.pawn + g.pose_state.offset) == kPoseAim;
    if (aiming && ue3::GetU8(lent.weapon_manager + g.current_mode.offset) == kModeChainsaw) {
      ue3::SetU8(lent.weapon_manager + g.current_mode.offset, kModeBlaster);
      g.mode_forced_on = lent.weapon_manager;
    }
  } else if (g.ammo_resolved && function == g.drop_ash_item) {
    lent.check = Check::EnemyDrop;
    lent.locals = locals;
    lent.drop_enabled = ue3::GetBool(object, g.enable_drop);
  } else if (function != g.get_driver_chainsaw) {
    return false;
  }
  g.stack.push_back(lent);
  return true;
}

void AfterScriptCallMoves(uint32_t result) {
  const Lent lent = g.stack.back();
  g.stack.pop_back();
  // ASH_EnemyPawn.DropAshItem spawns the drops the level's ASH_ZombieFactoryArchetypeAI lists gave the enemy when ASH_GameInfo.bPermissionItemSpawn[3] (other items) allows; stages before the grant list no bullets.
  if (lent.check == Check::EnemyDrop) {
    const uint32_t game_info = ue3::GetU32(lent.locals + g.drop_game_info.offset);
    if (lent.drop_enabled && game_info && ItemSpawnPermitted(game_info, kPermissionOtherItem) && ue3::IsLive(g.player_pawn) && BeforeGrant(g.player_pawn)) AddBlasterAmmo(g.player_pawn);
    return;
  }
  // UnrealScript's `true` is the u32 1 execTrue (0x8226B4D0) stores.
  if (lent.check == Check::DriverChainsaw && result) ue3::SetU32(result, 1);
  if (lent.count_lent) ue3::SetU32(lent.pawn + g.roulette_count.offset, lent.count);
  if (lent.possession_lent) ue3::SetBool(lent.controller, g.possession, false);
  if (lent.blaster_lent) ue3::SetBool(lent.controller, g.revolver_enabled, false);
  if (lent.check == Check::PlayerTick && g.mode_forced_on == lent.weapon_manager && ue3::GetU8(lent.pawn + g.pose_state.offset) != kPoseAim && ue3::GetU8(lent.weapon_manager + g.current_mode.offset) == kModeBlaster) {
    ue3::SetU8(lent.weapon_manager + g.current_mode.offset, kModeChainsaw);
    g.mode_forced_on = 0;
  }
}

}  // namespace cherry::engine
