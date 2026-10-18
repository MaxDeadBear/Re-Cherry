#pragma once

#include <filesystem>
#include <functional>

namespace cherry::ui {

// The options screen's GRAPHICS page (docs/ui/option-menu.md): its OK sets the graphics cvars through `run_on_ui_thread`, as the settings overlay does, and saves them to `config_path`.
void InitOptionMenu(const std::filesystem::path& config_path, std::function<void(std::function<void()>)> run_on_ui_thread);

}  // namespace cherry::ui
