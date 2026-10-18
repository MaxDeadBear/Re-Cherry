#include "app/debug/scripted_run.h"

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(std::string, cherry_debug_press);

namespace cherry::app::debug {

void IsolateScriptedRun(rex::PathConfig& paths) {
  namespace fs = std::filesystem;
  if (REXCVAR_GET(cherry_debug_press).empty() || paths.user_data_root.empty()) return;
  const fs::path sandbox = paths.user_data_root.parent_path() / (paths.user_data_root.filename().string() + "_scripted");
  std::error_code ec;
  fs::remove_all(sandbox, ec);
  fs::create_directories(sandbox, ec);
  // Saves, profiles and achievements; the SDK's shader cache is large and only ever rebuilt, so it is left out.
  for (const fs::directory_entry& entry : fs::directory_iterator(paths.user_data_root, ec)) {
    if (entry.path().filename() == "cache") continue;
    fs::copy(entry.path(), sandbox / entry.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  }
  const fs::path config = sandbox / paths.config_path.filename();
  if (fs::exists(paths.config_path, ec)) fs::copy_file(paths.config_path, config, fs::copy_options::overwrite_existing, ec);
  REXLOG_WARN("[scripted] input is scripted: saves and config are a copy in {}", sandbox.string());
  paths.user_data_root = sandbox;
  paths.config_path = config;
}

}  // namespace cherry::app::debug
