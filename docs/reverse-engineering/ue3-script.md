# UE3 objects and UnrealScript state

Lollipop Chainsaw keeps its gameplay state, including the combos and abilities bought in the Chop2 Shop, in UnrealScript properties, not in native code. `src/engine/ue3_objects.*` reads those properties through the engine's own reflection data. Every offset below was confirmed in IDA (`assets/default.xex.i64`) and then checked against a runtime dump.

## Reflection layout

| what | value | IDA source |
| --- | --- | --- |
| `GObjObjects` (TArray<UObject*>: data, num, max) | `0x83367248` | `UObject::AddObject` 0x822B38B8 stores `Data[Index] = this` (0x822B398C) |
| `FName::Names` (TArray<FNameEntry*>) | `0x83367208` | `FName::Init` 0x822A1E38 appends through 0x822A5DA0, which grows `0x8336720C` / `0x83367210` |
| FName name hash, 4096 buckets | `0x832EB170` | `FName::DisplayHash` 0x822A58C8 |
| `FNameEntry` | `+0` index << 1 with bit 0 set for UTF-16, `+4` hash next, `+8` text | 0x822A1E38 (`*entry >> 1`, `strcmp(entry + 8)` when bit 0 is clear) |
| FName | 8 bytes: index, number | compared as one doubleword in `StaticFindObjectFastInternal` 0x822AE450 |
| `UObject` | `+0x04` Index, `+0x08` ObjectFlags (u64), `+0x10` hash next, `+0x14` outer-hash next, `+0x28` Outer, `+0x2C` Name, `+0x34` Class | 0x822AE450 and 0x822B38B8 |
| `RF_ClassDefaultObject` | `0x200` in the low flags word (`+0x0C`) | the `set` console command 0x822AFAF0 |
| `UField::Next` | `+0x3C` | `FindField` 0x8222E568 |
| `UStruct::SuperField`, `Children` | `+0x40`, `+0x44` | the class walk in 0x822AE450; `TFieldIterator` 0x8222C650 / 0x8222DBD8 |
| `UStruct::Script` (TArray<BYTE>) | `+0x4C` | `execJumpIfNot` 0x82269F30 sets `Code = Node->Script + offset` (0x82269FA8) |
| `UClass::ClassCastFlags` | `+0xCC` | 0x8222DBD8 (`& 0x8000` UProperty), `FFrame::ReadVariableSize` 0x82282228 |
| `UProperty::ArrayDim`, `ElementSize`, `PropertyFlags`, `Offset` | `+0x40`, `+0x44`, `+0x48` (u64), `+0x54` | 0x82282228 (`ArrayDim * ElementSize`), 0x822AFAF0 (`object + prop[+0x54]`) |
| `UBoolProperty::BitMask` | `+0x74` | 0x822173B8: `0x20000` cast check, then `*(obj + Offset) & *(prop + 0x74)` |
| `UArrayProperty::Inner`, `UStructProperty::Struct` | `+0x74` | 0x8225C878: `0x200000` then `0x100000` cast checks reading `+0x74` |
| `UFunction` flags, native thunk | `+0x80`, `+0x94` | `UObject::CallFunction` 0x8227D538 (`0x400` native, else builds an `FFrame`) |
| `FFrame` | `+0x10` Node, `+0x14` Object, `+0x18` Code, `+0x1C` Locals | 0x8227D538, `UObject::ProcessInternal` 0x8227DA48 |

Bool values come in two forms. Bool fields and bool parameters use the property's bit mask; on this big-endian build the first bitfield is `0x80000000`. `CallFunction` 0x8227D538 normalises nonzero bool arguments to that mask. A bool return value is instead the u32 `0` or `1` that `execTrue` 0x8226B4D0 and `execBoolVariable` 0x82269480 write. The Scaleform `ExternalInterface` callback 0x829339B0, which is how Flash calls `ASHUS_*` functions, turns a nonzero bool return into `0x80000000` before converting it for ActionScript.

Bytecode pointers are 8 bytes, big-endian, with the address in the low word; FNames in bytecode are 8 bytes. `EX_EndOfScript` is `0x53`.

## Hooks

- `FEngineLoop::Tick` 0x829BD280 (game thread, calls `GEngine->Tick` through vtable `+284`) is hooked in `src/engine/game_tick.cpp`. Per-frame work that touches script objects runs after it returns.
- `UObject::ProcessInternal` 0x8227DA48 runs every UnrealScript function body. `CallFunction` 0x8227D538 and `execDynArraySort` 0x82268FC8 call it directly; `ProcessEvent` 0x8227DC30 (UObject vtable `+0xF8`) reaches it through `UFunction::Func` (`+0x94`). It is hooked in `src/engine/script_calls.cpp`, which hands each call to the features below.

## Dumping script classes

`--cherry_debug_dump_script_classes=true` writes these files to the working directory 600 engine ticks after start, which is the title screen:

- `script_classes.txt`: every `ASH*` class and script struct with its fields, and the live instances of each class.
- `script_bytecode.txt`: the bytecode of every function in those classes and their states.
- `script_objects.txt` and `script_names.txt`: the object and name tables.

`--cherry_debug_dump_script_values=ClassA,ClassB` also writes every property value of those classes' live instances to `script_values.txt`. `python -X utf8 tools/ue3script/disasm.py <folder> ASH_PlayerPowerUpLV.IsUnlockTechnique` prints decompiled-ish statements.

## Combos and abilities

The state lives on the single transient `ASH_PlayerPowerUpLV` object, which `ASH_IngameHUD.PlayerFlgMng` holds. Script reaches it through `GetUI().GetPlayerFlgList()`.

| property | meaning |
| --- | --- |
| `mUnlockTechFlag` (`SUnlockTechFlag`, `+0x4C`) | 24 bools `mKey_004` ... `mKey_036` in the word at `+0x50`, one per combo (`EUnlockTech`) |
| `mNickPopperUse`, `mNickShakeUse`, `mNickRacketUse`, `mNickShootUse` | Nick abilities (`+0x48`, masks `0x80000000` to `0x10000000`) |
| `mNick*UP` (Popper, Shake, Racket, Around, Shoot, Angel) | Nick ability upgrades (`+0x48`, `0x08000000` to `0x00400000`) |
| `mJuli_PowerUpLv[4]` | Juliet's stat levels: vitality (max 10), strength, homing, return (max 3) |
| `mSkl_*` | never read; only `InitDataLoad` clears them |

Combos are gated by `IsUnlockTechnique(key)`. `ASH_JulietPawn.SetDelayComboInfo` and the charge, dodge and leapfrog paths call it. The Nick flags are read directly by `ASH_JulietPawn` and the Nick roulette HUD.

The save keeps them in `ASH_SaveData.mAshGameSaveData.mPlayerData`: `mUnlockTech[0..23]`, `mUnlockNick[0..3]`, `mPowerUpNick[0..5]` and `mPowerUpAbility[0..3]`.

- `LoadGameFromMemory_PlayerData` copies the save to the flags.
- `SaveGame_PlayerData` copies the flags to the save, just before the native `ASH_GameEngine.SaveData` (`execSaveData` 0x829D7DA8, vtable `+464`) writes the file.
- The flags are changed only by:
  - `InitDataLoad`, `InitUnlockTechnique` and `InitUnlockTechnique_verGC` (defaults)
  - `ShopUnlockTechFlag` and `ASH_ShopItemDataList.SetItemTableSoldOut` (Chop2 Shop purchases)
  - `ASH_SeqAct_StageStart.Activated` (Nick's popper from stage 3, shoot from stage 4)
  - `FullUnlockTechnique` (no script caller)

### `debug_unlock_all_combos`

With the cvar on, the game-thread tick sets every `mUnlockTechFlag` key and every `mNick*` flag on the live `ASH_PlayerPowerUpLV` each frame. The values the game really has are kept on the host.

Whenever one of the functions that load, save, initialise or buy these flags runs, the `ProcessInternal` hook first puts the real values back and takes the new real values when it returns. That covers every writer listed above, `SaveGame_PlayerData` and `AllComboOpenSearch`. As a result:

- the save only ever receives what was really bought or unlocked;
- the "all combos" and "full power-up" achievements are judged on the real values;
- purchases made with the cvar on are kept.

Turning the cvar off restores the real values.

The flags object is not created once per session. Every map's `ASH_GameInfo.PreBeginPlay` creates a new `ASH_IngameHUD`. Its `Initialize` creates a new `ASH_PlayerPowerUpLV` and calls `InitDataLoad` on it. The cvar therefore switches to whichever `ASH_PlayerPowerUpLV` last ran one of its own functions; until then it uses the first live instance.

Juliet's stat levels (`mJuli_PowerUpLv`) and the shop's own sold-out table are not touched. Using a combo that the shop sells still runs `ASH_GameInfo.Archive_funcShopCombo`, which unlocks the "use a shop combo" achievement, just as it would for a bought combo.

### `debug_unlock_all_moves`

Ownership is not the only gate. Some moves are also gated by the story or the map:

| move | gate | where |
| --- | --- | --- |
| Chainsaw Dash | `ASH_PlayerController.OnButtonRB` enters `ChainsawDashStart` only if `Juliet.GetDriverChainsawFlag()` is true. On `STAGE00_PL`, `STAGE01_PL` and `Boss_Zed_PL` (and their shortcut/PIE names) that function returns `mDriverChainsawFlag` (`+0x6EC`, mask `0x800000`); everywhere else it returns true. | The flag is set only by the Kismet action `ASH_SeqAct_DriverChainsawSet`, and it is not saved. |
| Nick roulette, and so Nick Popper, Shake, Racket and Shoot | `ASH_PlayerController.OnLeftThumbstick` needs `Juliet.CheckNickTriggerStart()`, which fails when `NickRouletteCount` (`+0x1FF8`, the roulette tickets) is 0. `BeginNickRoulette` then also needs `bNickPossession` (`+0xDF4`, mask `0x20000000`). | Kismet sets the possession flag through `ASH_GameInfo.SetNickPossession`. `ASH_SeqAct_StageStart` gives 0 tickets on stage 0 and 1 elsewhere. The tickets are saved as `mPlayStage.mNickRouletteCount`. |

Chainsaw Blaster (the gun on LT) has its own gate:

- `ASH_PlayerController.OnButtonLT` only records the press.
- `PlayerTick` raises the shot pose (`ShotPoseIn`) only if `Juliet.TestRevolverStartStatus()` is true. That needs `mJulietController.mIsEnableRevolverWeapon` (`+0xDEC`, mask `0x1`), Juliet idle or moving, and no turkey state. When the flag is false, `PlayerTick` shows alert 2 instead.
- `ASH_GameInfo.InitStageStart` sets the flag to true at every stage start, and the level's Kismet action `ASH_SeqAct_RevolverWeapon` turns it on or off.
- The flag is not saved.
- `PlayerTick` reaches the whole LT branch only while `Juliet.WeaponManager.CurrentMode` (`ASH_WeaponManager +0x2C8`, enum `WeaponModes`) is 1, the Blaster weapon (`GetCurrentModeRef` returns `WeaponRevolver`). In mode 0 (`WeaponChainsaw`, set by `ASH_WeaponManager.PostBeginPlay`) only the lock-on code runs.
- The only script writer of `CurrentMode` is `ASH_WeaponModeRevolver.SetMode`, which Nick Popper uses: it saves the current mode, switches to 1 in cracker mode, and restores it afterwards. So the story's switch to the Blaster mode happens in level data, not script. The mode-1 branch of `PlayerTick` has its own lock-on code, and `TestChainsawDashAttackHit` handles mode 1, so normal play after the Blaster is granted runs in mode 1.
- Raising the gun: `PlayerTick` calls `ChangePoseState(3)`, which calls `SetRevolverChainsawParam`, then `WeaponRevolver.Begin`, then `BeginRevolver`, which shows the pistol meshes. Then it enters `ShotPoseIn`, which plays the unstow animation and returns to `Idle` with the pose still 3.
- `PlayerTick` clears the stance (`ClearRevolverStance`, then `ChangePoseState(1)` and `WeaponRevolver.End`) once LT drops below 0.3, Juliet leaves the states `TestRevolverStatus` accepts, or `Heat` reaches 1.
- The pistol meshes (`WP_Chainsaw.Meshes.SK_WP_Chainsaw_Revolver_*`) and `UI_RevolverReticle` are already loaded on the title map. Only `STAGE02_PL` lists `galaxy_newweapon_revolver` and `CS_RevolverForm_kis`, the cutscene that grants the gun.

Blaster ammo:

- The ammo count is `ASH_WeaponModeRevolver.mShotBulletNum`. `CanRevolverFire` needs it above 0 (the cracker ignores it). `AddBulletNum` and `SetBulletNum` cap it at `ASH_JulietPawn.fConfig_Juliet_Ammo_Max`.
- `ASH_GameInfo.InitStageStart` sets it to 100 outside boss stages, and `ASH_SeqAct_StageStart` sets it to 0 in ranking mode. The save keeps it as `mPlayStage.mBullet`.
- Pickups are `ASH_Item_Bullet` archetypes. `Pickup` adds `Bullet_AmountType{S,M,L}_Count`, chosen by `BulletAmountType`.
- Enemies drop through `ASH_EnemyPawn.DropAshItem`, when `bEnableDropItem` is set and `ASH_GameInfo.bPermissionItemSpawn[3]` allows "other items" (Kismet sets it with `ASH_SeqAct_PermissionItemSpawn`).
- What an enemy drops comes from `ItemList`, which `ASH_ZombieFactoryArchetypeAI.PostCreateActor` fills from the level's `ArchetypeZombieList[].DropItemList`. Stages before the grant simply list no bullets.
- The `IT_Bullet` package with `IT_Bullet.Archetypes.AR_BulletS_Cracker_Drop` (and its mesh) is already loaded on the title map.

Which items the roulette offers is decided separately, by `ASH_GFxHUDNickRoulette.ASHUS_GetNickRouletteItemUse` from the `mNick*Use` flags. `debug_unlock_all_combos` forces those.

The cvar only lends state for the length of these checks, through the `ProcessInternal` hook:

- `GetDriverChainsawFlag` has its result overwritten with UnrealScript's `true`, the u32 `1` that `execTrue` (0x8226B4D0) stores.
- During `CheckNickTriggerStart` and `BeginNickRoulette`, a ticket count of 0 or less reads as 1.
- During `BeginNickRoulette`, a missing `bNickPossession` reads as true.
- During `TestRevolverStartStatus`, a false `mIsEnableRevolverWeapon` reads as true.
- When `PlayerTick` runs with LT above 0.3 or the aim pose up, a `CurrentMode` of 0 becomes 1. It goes back to 0 after the first `PlayerTick` that ends without the aim pose, so melee outside aiming stays in the stage's own mode.
- When `DropAshItem` runs for an enemy with `bEnableDropItem` while other-item drops are permitted and the stage has not granted the Blaster (its own weapon mode is 0, or 1 only because the cvar raised it), the player gets what picking up `AR_BulletS_Cracker_Drop` gives, capped at the ammo maximum. No pickup actor is spawned, because adding one to the enemy's `ItemList` would need a guest allocation.
- Both values go back as soon as the call returns. A roulette opened with no tickets therefore leaves the count at 0, and one opened with tickets uses one as usual.

Nothing it changes outlives the call, so nothing reaches the save.

It is a separate cvar from `debug_unlock_all_combos` because it overrides story context rather than shop ownership. Where the story has Nick away, his head stays hidden while his moves play.

### Nick roulette path and `--cherry_debug_log_nick`

The path from the input to the Nick Popper move:

1. `ASH_PlayerController.OnLeftThumbstick` runs while the player state is `PlayerWalking`, or `AttackingWait` with `TestAttackOnlyStatus`, and `bActionUse` is false.
2. It calls `Juliet.CheckNickTriggerStart()`. This needs tickets and no cinematic, galaxy, star-mode, Nick-intro or UI lock state.
3. `BeginNickRoulette` needs `bNickPossession`, an unlocked menu and Juliet alive. It uses a ticket.
4. The roulette HUD's `ASHUS_CheckInitFrameNickReticle` chooses the slot list `NRList_0`..`NRList_6` or `NRList_Maris` from Juliet's health, the enemy count, the death roulette or the Mariska fight. It passes the list to the Flash movie through `CallAS_StartNickRoulette`.
5. The movie asks `ASHUS_GetNickRouletteItemUse(nHit)` about each slot. Slot 5 is `NR_NICKPOPPER`, which needs `mNickPopperUse`. `bNickRouletteUse` on the HUD makes every slot usable.
6. When the wheel stops, `ASHUS_HitNickRouletteEnd(nHit)` turns slots 1 and 4 into 0 (miss). It then calls `NickRouletteAction(nHit)`.
7. For move 5 that leads to `GotoStateNickIntro`, the `NickRouletteIntro` state, its `OnAnimEnd`, then `GotoStateNickPoper`.
8. `GotoStateNickPoper` sets `NickShakeLoadEventName = 'WeaponChangeCracker'`, `NickShakeWarpEventName = 'WeaponChangeWarp_Cracker'` and `NickGalaxyLevelName = 'galaxy_nickskill_Poper'`, then enters the `NickHangs` state.
9. `NickHangs.BeginState` fires the remote event `WeaponChangeCracker`. `NickShakeLoading.Tick` then waits for `mbWeaponLoadFlag`, which no script code sets to true, so the level's own sequence must set it.

None of these steps checks the stage number. `STAGE01_PL`, `STAGE02_PL`, `STAGE02_BOSS_PL`, `Stage03_PL`, `STAGE04_PL`, `STAGE05_PL` and `Stage06_PL` all list `galaxy_nickskill_Poper`; `STAGE00_PL` lists none of the Nick skill levels.

`--cherry_debug_log_nick=true` (in `src/engine/debug/nick_trace.cpp`) logs each of these functions with `[nick] >` (arguments) and `[nick] <` (return value), along with `OnButtonRB` and `GetDriverChainsawFlag` for Chainsaw Dash. At the gate functions it also logs:

- each player controller's `bNickPossession`, `bActionUse` and roulette state;
- Juliet's tickets, `mDriverChainsawFlag`, `mbWeaponLoadFlag` and other gate fields;
- every `ASH_IngameHUD` and the `PlayerFlgMng` it holds;
- every live `ASH_PlayerPowerUpLV` with its `mNick*` flags;
- how many loaded `SeqEvent_RemoteEvent` objects answer Juliet's current load and warp events.
