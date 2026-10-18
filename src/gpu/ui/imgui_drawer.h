// The ImGui bridge to the native renderer: ReXApp builds no overlays for a graphics system without a presenter, so the app drives an ImGuiDrawer on the UI thread through this drawer, which hands each finished frame to GraphicsSystem::SubmitUi.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>

#include "gpu/graphics_system.h"

namespace cherry::gpu {

// UIDrawContext's presenter-less constructor is protected, so app-driven overlays need a subclass to reach it.
class UiDrawContext final : public rex::ui::UIDrawContext {
 public:
  UiDrawContext(uint32_t width, uint32_t height) : rex::ui::UIDrawContext(width, height) {}
};

class PlumeImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit PlumeImmediateDrawer(GraphicsSystem* graphics) : graphics_(graphics) {}

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter filter, bool is_repeated, const uint8_t* data) override;
  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width, float coordinate_space_height) override;
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override;
  void Draw(const rex::ui::ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  GraphicsSystem* graphics_ = nullptr;
  float space_width_ = 1.0f;
  float space_height_ = 1.0f;
  uint32_t target_width_ = 0;
  uint32_t target_height_ = 0;
  std::vector<rex::ui::ImmediateVertex> batch_vertices_;
  std::vector<uint16_t> batch_indices_;
  GraphicsSystem::UiFrame frame_;
};

}  // namespace cherry::gpu
