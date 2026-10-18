#pragma once

#include <filesystem>

namespace cherry::engine {

// Applies the costume_slot_NN cvars now and whenever they change: -1 restores the original file, 0-36 copies that source costume over the slot.
// Sources come from skin_mods_dir first, then cooked_dir; the game only ever reads cooked_dir.
void InitCostumes(const std::filesystem::path& cooked_dir, const std::filesystem::path& skin_mods_dir);

}  // namespace cherry::engine
