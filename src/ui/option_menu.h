#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace cherry::ui {

// The options screen's GRAPHICS page (docs/ui/option-menu.md): its OK sets the graphics cvars through `run_on_ui_thread`, as the settings overlay does, and saves them to `config_path`.
void InitOptionMenu(const std::filesystem::path& config_path, std::function<void(std::function<void()>)> run_on_ui_thread);

// The options screen's movie with the GRAPHICS page's script after its controller's (src/ui/actionscript/option_menu_graphics.as); empty for any other movie.
std::vector<uint8_t> AddGraphicsPage(const std::vector<uint8_t>& movie);

}  // namespace cherry::ui
