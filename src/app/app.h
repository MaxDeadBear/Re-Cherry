// The game's ReXApp: the costume swapper and the ImGui overlays.
#pragma once

#include <filesystem>
#include <memory>

#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace cherry::gpu {
class GraphicsSystem;
}  // namespace cherry::gpu

namespace cherry::ui {
class OverlayStack;
}  // namespace cherry::ui

class ReCherryApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);
  ~ReCherryApp() override;

  void OnPreSetup(rex::RuntimeConfig& config) override;
  bool SetupPresentation() override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnShutdown() override;
  void OnConfigurePaths(rex::PathConfig& paths) override;

 private:
  cherry::gpu::GraphicsSystem* graphics_ = nullptr;
  std::filesystem::path config_path_;
  std::unique_ptr<cherry::ui::OverlayStack> overlays_;
  std::unique_ptr<rex::ui::ImGuiDialog> fps_overlay_;
};
