
#pragma once
#include <filesystem>

// Call from OnPostSetup() to register change callbacks for all costume CVARs.
// Slots costume_slot_00 through costume_slot_24 appear in Lollipop/Costumes.
// Value -1 = original file, 0-36 = source slot to copy over the destination.
//
// cooked_dir  : CookedXbox360 — destination files live here (game reads them).
// skin_mods_dir: SkinMods     — source mod files live here (never touched by UE3).
void InitCostumeManager(const std::filesystem::path& cooked_dir,
                        const std::filesystem::path& skin_mods_dir);
