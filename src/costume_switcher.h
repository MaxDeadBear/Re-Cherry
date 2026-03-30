
#pragma once
#include <filesystem>

// Call from OnPostSetup() to register change callbacks for all costume CVARs.
// Slots costume_slot_00 through costume_slot_24 appear in Lollipop/Costumes.
// Value -1 = original file, 0-36 = source slot to copy over the destination.
void InitCostumeManager(const std::filesystem::path& cooked_dir);
