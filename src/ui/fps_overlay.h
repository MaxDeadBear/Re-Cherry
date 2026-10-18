#pragma once

#include <rex/ui/imgui_dialog.h>

namespace cherry::ui {

// Corner FPS readout, shown while cherry_fps_counter is set: bind_fps_overlay (F12) and X on the options screen toggle it.
class FpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer);
  ~FpsOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace cherry::ui
