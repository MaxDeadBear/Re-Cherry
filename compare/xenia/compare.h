// Xenia side-by-side comparison mode (compare/xenia/README.md); only built with -DCHERRY_XENIA_COMPARE=ON.
#pragma once

namespace rex {
struct RuntimeConfig;
}  // namespace rex
namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace cherry::compare {

// Leaves the runtime's graphics to the SDK's Xenia GPU plugin, and starts the native renderer in a second window.
void ConfigureRuntime(rex::RuntimeConfig& config);
// Takes the game's window out of fullscreen and puts it beside the native renderer's: Xenia on the left half of the screen, native on the right.
void ArrangeWindows(rex::ui::Window* game_window);
void Shutdown();

}  // namespace cherry::compare
