
#include "costume_switcher.h"
#include <charconv>
#include <format>
#include <filesystem>
#include <rex/cvar.h>
#include <rex/logging/macros.h>

// ---------------------------------------------------------------------------
// CVARs — one per destination slot (00-24), shown in F4 > Lollipop/Costumes.
// Value: -1 = use original file, 0-36 = copy that source slot over destination.
// ---------------------------------------------------------------------------
#define COSTUME_CVAR(n) \
    REXCVAR_DEFINE_INT32(costume_slot_##n, -1, "Lollipop/Costumes", \
        "Costume " #n " source slot (-1 = original, 0-36 = override)") \
        .range(-1, 36)

COSTUME_CVAR(00);
COSTUME_CVAR(01);
COSTUME_CVAR(02);
COSTUME_CVAR(03);
COSTUME_CVAR(04);
COSTUME_CVAR(05);
COSTUME_CVAR(06);
COSTUME_CVAR(07);
COSTUME_CVAR(08);
COSTUME_CVAR(09);
COSTUME_CVAR(10);
COSTUME_CVAR(11);
COSTUME_CVAR(12);
COSTUME_CVAR(13);
COSTUME_CVAR(14);
COSTUME_CVAR(15);
COSTUME_CVAR(16);
COSTUME_CVAR(17);
COSTUME_CVAR(18);
COSTUME_CVAR(19);
COSTUME_CVAR(20);
COSTUME_CVAR(21);
COSTUME_CVAR(22);
COSTUME_CVAR(23);
COSTUME_CVAR(24);

// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

// src_file is resolved from skin_mods_dir (never touched by UE3).
// dest_file and its backup live in cooked_dir (what the game reads).
static void ApplyCostumeOverride(int dest, int src,
                                 const fs::path& cooked_dir,
                                 const fs::path& skin_mods_dir) {
    auto dest_file = cooked_dir  / std::format("costume{:02d}.xxx", dest);
    auto backup    = cooked_dir  / std::format("costume{:02d}.xxx.bak", dest);
    std::error_code ec;

    if (src == -1) {
        if (!fs::exists(backup)) {
            REXLOG_INFO("[costumes] costume{:02d}: no backup, nothing to restore", dest);
            return;
        }
        fs::copy_file(backup, dest_file, fs::copy_options::overwrite_existing, ec);
        if (ec) { REXLOG_WARN("[costumes] costume{:02d}: restore failed: {}", dest, ec.message()); return; }
        REXLOG_INFO("[costumes] costume{:02d}: restored original", dest);
        fs::remove(backup, ec);
        if (ec) REXLOG_WARN("[costumes] costume{:02d}: failed to delete backup: {}", dest, ec.message());
        return;
    }

    // Look for the source in SkinMods first, fall back to CookedXbox360.
    auto src_file = skin_mods_dir / std::format("costume{:02d}.xxx", src);
    if (fs::exists(src_file)) {
        REXLOG_INFO("[costumes] costume{:02d}: source {:02d} found in SkinMods ({})", dest, src, src_file.string());
    } else {
        src_file = cooked_dir / std::format("costume{:02d}.xxx", src);
        if (!fs::exists(src_file)) {
            REXLOG_WARN("[costumes] costume{:02d}: source costume{:02d}.xxx not found in SkinMods ({}) or CookedXbox360",
                        dest, src, (skin_mods_dir / std::format("costume{:02d}.xxx", src)).string());
            return;
        }
        REXLOG_INFO("[costumes] costume{:02d}: source {:02d} found in CookedXbox360", dest, src);
    }

    if (!fs::exists(backup)) {
        fs::copy_file(dest_file, backup, ec);
        if (ec) { REXLOG_WARN("[costumes] costume{:02d}: backup failed: {}", dest, ec.message()); return; }
    }

    fs::copy_file(src_file, dest_file, fs::copy_options::overwrite_existing, ec);
    if (ec) REXLOG_WARN("[costumes] costume{:02d}: apply failed: {}", dest, ec.message());
    else    REXLOG_INFO("[costumes] costume{:02d}: applied source {:02d}", dest, src);
}

void InitCostumeManager(const fs::path& cooked_dir,
                        const fs::path& skin_mods_dir) {
    for (int i = 0; i <= 24; ++i) {
        std::string cvar_name = std::format("costume_slot_{:02d}", i);

        // Register callback for future changes via the settings overlay
        rex::cvar::RegisterChangeCallback(cvar_name,
            [i, cooked_dir, skin_mods_dir](std::string_view, std::string_view new_val) {
                int src = -1;
                std::from_chars(new_val.data(), new_val.data() + new_val.size(), src);
                ApplyCostumeOverride(i, src, cooked_dir, skin_mods_dir);
            });

        // Apply any value already set (e.g. loaded from config on startup)
        std::string cur = rex::cvar::GetFlagByName(cvar_name);
        int src = -1;
        std::from_chars(cur.data(), cur.data() + cur.size(), src);
        if (src != -1) {
            ApplyCostumeOverride(i, src, cooked_dir, skin_mods_dir);
        }
    }
}
