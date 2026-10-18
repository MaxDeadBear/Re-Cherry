// The SDK's overlay stack (F3 debug, F4 settings, backtick console) on the native renderer, which has no presenter for ReXApp to build it on.
// ImGui runs on the UI thread only: each present asks for a frame, the UI thread builds it and the render thread draws the latest one (docs/renderer/README.md).
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>

#include <rex/logging/sink.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "gpu/graphics_system.h"
#include "gpu/ui/imgui_drawer.h"

namespace cherry::ui {

class OverlayStack {
 public:
  OverlayStack(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window, gpu::GraphicsSystem* graphics, std::filesystem::path config_path);
  ~OverlayStack();

  rex::ui::ImGuiDrawer* drawer() const { return drawer_.get(); }
  // For InputSystem::SetActiveCallback: false while an SDK overlay is open and ImGui wants the mouse or keyboard; callable from any thread.
  std::function<bool()> GameInputActive() const;

 private:
  // Shared with the render thread's frame request and the input callback, which can outlive the stack.
  struct Pump {
    std::atomic<bool> queued{false};
    std::atomic<bool> imgui_takes_input{false};
    OverlayStack* stack = nullptr;  // UI thread only
  };

  void DrawFrame();

  rex::ui::Window* window_ = nullptr;
  gpu::GraphicsSystem* graphics_ = nullptr;
  std::filesystem::path config_path_;
  std::shared_ptr<rex::LogCaptureSink> log_sink_;
  std::shared_ptr<Pump> pump_;
  std::unique_ptr<gpu::PlumeImmediateDrawer> immediate_drawer_;
  std::unique_ptr<rex::ui::ImGuiDrawer> drawer_;
  std::unique_ptr<rex::ui::ImGuiDialog> debug_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> console_overlay_;
  std::unique_ptr<rex::ui::ImGuiDialog> settings_overlay_;
};

}  // namespace cherry::ui
