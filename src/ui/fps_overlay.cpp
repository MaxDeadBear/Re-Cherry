#include "ui/fps_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/keybinds.h>

#include "engine/frame_rate.h"

REXCVAR_DEFINE_BOOL(cherry_fps_counter, false, "Lollipop/UI", "Show the FPS counter in the picture's top-right corner; F12 or X on the options screen toggles it.");
REXCVAR_DECLARE(bool, cherry_gfx_stretch);

namespace cherry::ui {

namespace {

// Drawn in the style of the HUD's SCORE banner: a tilted orange card with halftone dots, a heavy black outline and a hard shadow, and chunky slanted block lettering outlined in black.
constexpr ImU32 kInk = IM_COL32(16, 10, 8, 255);
constexpr ImU32 kCard = IM_COL32(243, 160, 62, 255);
constexpr ImU32 kCardDots = IM_COL32(214, 104, 34, 255);
constexpr ImU32 kLabel = IM_COL32(220, 38, 36, 255);
constexpr ImU32 kDigits = IM_COL32(255, 250, 240, 255);
constexpr float kTilt = -0.07f;  // radians
constexpr float kSlant = 0.22f;  // glyph italic: x shift per unit of height

// Segments a..g of a seven-segment cell, as bits 0..6.
uint8_t GlyphSegments(char c) {
  switch (c) {
    case '0': return 0x3F;
    case '1': return 0x06;
    case '2': return 0x5B;
    case '3': return 0x4F;
    case '4': return 0x66;
    case '5': case 'S': return 0x6D;
    case '6': return 0x7D;
    case '7': return 0x07;
    case '8': return 0x7F;
    case '9': return 0x6F;
    case 'F': return 0x71;
    case 'P': return 0x73;
    default: return 0;
  }
}

// Maps card-local points (origin at the card's centre, in game pixels) to the screen, tilted with the card.
struct Card {
  ImVec2 centre;
  float unit;
  float cos_t = std::cos(kTilt);
  float sin_t = std::sin(kTilt);
  ImVec2 operator()(float x, float y) const { return ImVec2(centre.x + (x * cos_t - y * sin_t) * unit, centre.y + (x * sin_t + y * cos_t) * unit); }
};

void Quad(ImDrawList* dl, const Card& card, const ImVec2 (&local)[4], ImVec2 offset, ImU32 colour) {
  ImVec2 points[4];
  for (int i = 0; i < 4; ++i) points[i] = card(local[i].x + offset.x, local[i].y + offset.y);
  dl->AddConvexPolyFilled(points, 4, colour);
}

// One block segment from (x0, y0) to (x1, y1) in a glyph whose top-left is `origin` and height `h`, slanted, grown by `grow` on every side.
void Segment(ImDrawList* dl, const Card& card, ImVec2 origin, float h, float x0, float y0, float x1, float y1, float grow, ImVec2 offset, ImU32 colour) {
  x0 -= grow;
  y0 -= grow;
  x1 += grow;
  y1 += grow;
  auto slant = [&](float x, float y) { return ImVec2(origin.x + x + kSlant * (h - y), origin.y + y); };
  const ImVec2 local[4] = {slant(x0, y0), slant(x1, y0), slant(x1, y1), slant(x0, y1)};
  Quad(dl, card, local, offset, colour);
}

void Glyph(ImDrawList* dl, const Card& card, ImVec2 origin, float w, float h, char c, float grow, ImVec2 offset, ImU32 colour) {
  const uint8_t s = GlyphSegments(c);
  const float t = h * 0.21f;
  const float mid = h * 0.5f;
  if (s & 0x01) Segment(dl, card, origin, h, 0, 0, w, t, grow, offset, colour);
  if (s & 0x02) Segment(dl, card, origin, h, w - t, 0, w, mid + t * 0.5f, grow, offset, colour);
  if (s & 0x04) Segment(dl, card, origin, h, w - t, mid - t * 0.5f, w, h, grow, offset, colour);
  if (s & 0x08) Segment(dl, card, origin, h, 0, h - t, w, h, grow, offset, colour);
  if (s & 0x10) Segment(dl, card, origin, h, 0, mid - t * 0.5f, t, h, grow, offset, colour);
  if (s & 0x20) Segment(dl, card, origin, h, 0, 0, t, mid + t * 0.5f, grow, offset, colour);
  if (s & 0x40) Segment(dl, card, origin, h, 0, mid - t * 0.5f, w, mid + t * 0.5f, grow, offset, colour);
}

float TextWidth(const char* text, float w, float gap) {
  const size_t n = std::strlen(text);
  return n ? n * w + (n - 1) * gap : 0.0f;
}

// Three passes so neighbouring glyphs share one outline: hard shadow, black outline, then the fill.
void Text(ImDrawList* dl, const Card& card, ImVec2 origin, float w, float h, float gap, const char* text, ImU32 colour) {
  const float outline = h * 0.12f;
  const ImVec2 shadow(h * 0.1f, h * 0.1f);
  for (int pass = 0; pass < 3; ++pass) {
    float x = origin.x;
    for (const char* c = text; *c; ++c, x += w + gap) {
      if (pass == 0) Glyph(dl, card, ImVec2(x, origin.y), w, h, *c, outline, shadow, kInk);
      if (pass == 1) Glyph(dl, card, ImVec2(x, origin.y), w, h, *c, outline, ImVec2(0, 0), kInk);
      if (pass == 2) Glyph(dl, card, ImVec2(x, origin.y), w, h, *c, 0.0f, ImVec2(0, 0), colour);
    }
  }
}

}  // namespace

FpsOverlay::FpsOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
  rex::ui::RegisterBind("bind_fps_overlay", "F12", "Toggle FPS overlay", [] { REXCVAR_SET(cherry_fps_counter, !REXCVAR_GET(cherry_fps_counter)); });
}

FpsOverlay::~FpsOverlay() {
  rex::ui::UnregisterBind("bind_fps_overlay");
}

void FpsOverlay::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(cherry_fps_counter) || io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;

  // The game picture: the whole window when stretched, else the 16:9 rectangle the present letterboxes it into.
  float picture_w = io.DisplaySize.x;
  float picture_h = io.DisplaySize.y;
  if (!REXCVAR_GET(cherry_gfx_stretch)) {
    picture_w = std::min(picture_w, picture_h * 16.0f / 9.0f);
    picture_h = picture_w * 9.0f / 16.0f;
  }
  // A small card tucked into the picture's top-right corner, above the SCORE banner.
  const float unit = picture_h / 720.0f * 0.55f;
  const float top = (io.DisplaySize.y - picture_h) * 0.5f;
  const float right = (io.DisplaySize.x + picture_w) * 0.5f;

  char digits[8];
  std::snprintf(digits, sizeof(digits), "%d", std::clamp(int(std::lround(cherry::engine::MeasuredFps())), 0, 9999));
  constexpr float kLabelW = 11.0f, kLabelH = 18.0f, kDigitW = 15.0f, kDigitH = 25.0f, kGap = 4.5f, kSpace = 10.0f, kPad = 14.0f;
  const float text_w = TextWidth("FPS", kLabelW, kGap - 1.0f) + kSpace + TextWidth(digits, kDigitW, kGap) + kSlant * kDigitH;
  const float card_w = std::max(text_w + 2.0f * kPad, 118.0f);
  const float card_h = 42.0f;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const Card card{ImVec2(right - (card_w * 0.5f + 22.0f) * unit, top + (card_h * 0.5f + 14.0f) * unit), unit};
  const float hw = card_w * 0.5f, hh = card_h * 0.5f, lean = 7.0f;
  const ImVec2 body[4] = {{-hw + lean, -hh}, {hw + lean, -hh}, {hw - lean, hh}, {-hw - lean, hh}};
  const ImVec2 rim[4] = {{-hw + lean - 3.5f, -hh - 3.5f}, {hw + lean + 3.5f, -hh - 3.5f}, {hw - lean + 3.5f, hh + 3.5f}, {-hw - lean - 3.5f, hh + 3.5f}};
  Quad(dl, card, rim, ImVec2(5.0f, 5.0f), kInk);
  Quad(dl, card, rim, ImVec2(0, 0), kInk);
  Quad(dl, card, body, ImVec2(0, 0), kCard);
  // Halftone: dots on a diagonal grid, growing towards the card's right end like the SCORE banner's.
  constexpr float kStep = 5.5f;
  for (float y = -hh + kStep * 0.5f; y < hh; y += kStep) {
    const bool odd = int(std::lround((y + hh) / kStep)) & 1;
    for (float x = -hw - lean + (odd ? kStep * 0.5f : 0.0f); x < hw + lean; x += kStep) {
      const float edge = lean * (-y / hh);
      const float radius = 0.35f + 1.35f * std::clamp((x + hw) / (2.0f * hw), 0.0f, 1.0f);
      if (x - radius < -hw + edge + 1.0f || x + radius > hw + edge - 1.0f || std::fabs(y) + radius > hh - 1.0f) continue;
      dl->AddCircleFilled(card(x, y), radius * unit, kCardDots, 8);
    }
  }
  const float text_left = -text_w * 0.5f;
  Text(dl, card, ImVec2(text_left, kDigitH * 0.5f - kLabelH), kLabelW, kLabelH, kGap - 1.0f, "FPS", kLabel);
  Text(dl, card, ImVec2(text_left + TextWidth("FPS", kLabelW, kGap - 1.0f) + kSpace, -kDigitH * 0.5f), kDigitW, kDigitH, kGap, digits, kDigits);
}

}  // namespace cherry::ui
