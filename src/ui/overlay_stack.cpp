#include "ui/overlay_stack.h"

#include <imgui.h>

#include <rex/logging/api.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>

#include "engine/frame_rate.h"

namespace cherry::ui {

namespace {

// rexglue's overlays have no visibility of their own, so a toggle constructs or destroys one, as ReXApp's binds do.
template <typename Make>
void Toggle(std::unique_ptr<rex::ui::ImGuiDialog>& slot, Make make) {
  if (slot) slot.reset();
  else slot = make();
}

rex::ui::FrameStats GuestFrameStats() {
  const double fps = cherry::engine::MeasuredFps();
  return {fps > 0.0 ? 1000.0 / fps : 0.0, fps, cherry::engine::PresentedFrames()};
}

}  // namespace

OverlayStack::OverlayStack(rex::ui::WindowedAppContext& app_context, rex::ui::Window* window, gpu::GraphicsSystem* graphics, std::filesystem::path config_path) : window_(window), graphics_(graphics), config_path_(std::move(config_path)), pump_(std::make_shared<Pump>()) {
  pump_->stack = this;
  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);
  immediate_drawer_ = std::make_unique<gpu::PlumeImmediateDrawer>(graphics_);
  drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_, 64);
  // With no presenter, AddDialog only registers the drawer's window input listener; the frames are driven from DrawFrame.
  drawer_->SetPresenterAndImmediateDrawer(nullptr, immediate_drawer_.get());

  // The same bind names as ReXApp's, so keys saved under the SDK's overlays carry over; they arrive through ReXApp::OnKeyDown on the UI thread.
  rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] { Toggle(debug_overlay_, [this] { return std::make_unique<rex::ui::DebugOverlayDialog>(drawer_.get(), GuestFrameStats); }); });
  rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] { Toggle(console_overlay_, [this] { return std::make_unique<rex::ui::ConsoleDialog>(drawer_.get(), log_sink_); }); });
  rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] { Toggle(settings_overlay_, [this] { return std::make_unique<rex::ui::SettingsDialog>(drawer_.get(), config_path_); }); });

  // Runs on the render thread after each present; at most one frame is queued on the UI thread, so a busy UI thread only drops UI frames.
  graphics_->SetUiFrameRequest([pump = pump_, context = &app_context] {
    if (pump->queued.exchange(true)) return;
    context->CallInUIThread([pump] {
      pump->queued.store(false);
      if (pump->stack) pump->stack->DrawFrame();
    });
  });
}

OverlayStack::~OverlayStack() {
  graphics_->SetUiFrameRequest(nullptr);
  pump_->stack = nullptr;
  pump_->imgui_takes_input.store(false);
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
  drawer_.reset();
  immediate_drawer_.reset();
  graphics_->SubmitUi({});
}

std::function<bool()> OverlayStack::GameInputActive() const {
  return [pump = pump_] { return !pump->imgui_takes_input.load(std::memory_order_relaxed); };
}

void OverlayStack::DrawFrame() {
  if (!drawer_->HasDialogs()) {
    graphics_->SubmitUi({});
    pump_->imgui_takes_input.store(false);
    return;
  }
  gpu::UiDrawContext context(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  drawer_->Draw(context);
  const ImGuiIO& io = drawer_->GetIO();
  const bool sdk_overlay_open = debug_overlay_ || console_overlay_ || settings_overlay_;
  pump_->imgui_takes_input.store(sdk_overlay_open && (io.WantCaptureMouse || io.WantCaptureKeyboard), std::memory_order_relaxed);
}

}  // namespace cherry::ui
