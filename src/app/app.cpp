#include "app/app.h"

#include <filesystem>

#include "re_cherry_init.h"

#include <rex/filesystem.h>
#include <rex/input/input_system.h>

#include "app/debug/hang_watchdog.h"
#include "app/debug/sampling_profiler.h"
#ifdef CHERRY_XENIA_COMPARE
#include "compare/xenia/compare.h"
#endif
#include "engine/costumes.h"
#include "gpu/debug/screenshot.h"
#include "gpu/graphics_system.h"
#include "gpu/shaders/shader_registry.h"
#include "ui/fps_overlay.h"
#include "ui/option_menu.h"
#include "ui/overlay_stack.h"

ReCherryApp::~ReCherryApp() = default;

void ReCherryApp::OnConfigurePaths(rex::PathConfig& paths) {
  namespace fs = std::filesystem;
  config_path_ = paths.config_path;
  constexpr const char* kEntryXexName = "default.xex";
  std::error_code ec;
  if (fs::exists(paths.game_data_root / kEntryXexName, ec)) return;
  // With no --game_data_root, the nearest assets/ folder at or above the executable is the game, which covers both a dev build and a copied release.
  for (auto dir = rex::filesystem::GetExecutableFolder(); !dir.empty(); dir = dir.parent_path()) {
    if (fs::exists(dir / "assets" / kEntryXexName, ec)) {
      paths.game_data_root = dir / "assets";
      return;
    }
    if (dir == dir.parent_path()) return;
  }
}

std::unique_ptr<rex::ui::WindowedApp> ReCherryApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<ReCherryApp>(new ReCherryApp(ctx, "re_cherry", PPCImageConfig));
}

void ReCherryApp::OnPreSetup(rex::RuntimeConfig& config) {
#ifdef CHERRY_XENIA_COMPARE
  cherry::compare::ConfigureRuntime(config);
#else
  auto graphics = std::make_unique<cherry::gpu::GraphicsSystem>([this] { return cherry::gpu::NativeWindowHandle(window()); }, cherry::gpu::ConfiguredBackend());
  graphics_ = graphics.get();
  config.graphics = std::move(graphics);
  config.gpu_plugin.clear();
#endif
}

// ReXApp builds its overlays only for a graphics system with a presenter or for none, so the native renderer's are built here (ui/overlay_stack.h).
bool ReCherryApp::SetupPresentation() {
  if (!ReXApp::SetupPresentation()) return false;
#ifndef CHERRY_XENIA_COMPARE
  if (graphics_ && window() && !imgui_drawer()) {
    overlays_ = std::make_unique<cherry::ui::OverlayStack>(app_context(), window(), graphics_, config_path_);
    OnCreateDialogs(overlays_->drawer());
  }
#endif
  return true;
}

void ReCherryApp::OnPostSetup() {
#ifdef CHERRY_XENIA_COMPARE
  cherry::compare::ArrangeWindows(window());
#else
  cherry::gpu::debug::RegisterFrameBurstBind();
  // ReXApp mutes the game's keyboard and mouse input while its overlays hold ImGui's focus; this is the same for the native ones.
  if (overlays_ && runtime()->input_system()) static_cast<rex::input::InputSystem*>(runtime()->input_system())->SetActiveCallback(overlays_->GameInputActive());
#endif
  cherry::app::StartHangWatchdog();
  cherry::app::debug::StartSamplingProfiler();
  cherry::gpu::SetShaderDumpDirectory(game_data_root().parent_path() / "shader_dump");
  cherry::engine::InitCostumes(game_data_root() / "AshGame" / "CookedXbox360", game_data_root() / "SkinMods");
  cherry::ui::InitOptionMenu(config_path_, [this](std::function<void()> fn) { app_context().CallInUIThreadDeferred(std::move(fn)); });
}

void ReCherryApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  fps_overlay_ = std::make_unique<cherry::ui::FpsOverlay>(drawer);
}

void ReCherryApp::OnShutdown() {
#ifdef CHERRY_XENIA_COMPARE
  cherry::compare::Shutdown();
#endif
  cherry::app::debug::StopSamplingProfiler();
  cherry::app::StopHangWatchdog();
  fps_overlay_.reset();
  overlays_.reset();
}
