#include <cctype>
#include <string>

#include "re_cherry_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/flags.h>

REXCVAR_DEFINE_BOOL(disable_xbox_live_prompt, true, "Lollipop/Patches", "Auto-dismiss Xbox Live connection prompts");

namespace {

std::string ReadGuestUtf16Lower(uint8_t* base, uint32_t guest_ptr) {
  std::string out;
  if (!guest_ptr) return out;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint16_t ch = REX_LOAD_U16(guest_ptr + i * 2);
    if (ch == 0) break;
    out.push_back(ch <= 0x7F ? static_cast<char>(std::tolower(ch)) : '?');
  }
  return out;
}

bool IsXboxLivePrompt(uint8_t* base, uint32_t title_ptr, uint32_t text_ptr) {
  const std::string haystack = ReadGuestUtf16Lower(base, title_ptr) + "\n" + ReadGuestUtf16Lower(base, text_ptr);
  return haystack.find("xbox live") != std::string::npos || haystack.find("connect to xbox") != std::string::npos || haystack.find("xboxlive") != std::string::npos;
}

}  // namespace

// XShowMessageBoxUI(user_index, title, text, button_count, buttons, focus_button, style, result, overlapped)
REX_HOOK_RAW(XShowMessageBoxUI) {
  if (!REXCVAR_GET(disable_xbox_live_prompt) || !IsXboxLivePrompt(base, ctx.r4.u32, ctx.r5.u32)) {
    __imp__XShowMessageBoxUI(ctx, base);
    return;
  }
  // A headless message box returns the focused button at once; multi-button prompts are answered with button 1.
  const bool old_headless = REXCVAR_GET(headless);
  REXCVAR_SET(headless, true);
  if (ctx.r6.u32 >= 2) ctx.r8.u32 = 1;
  __imp__XShowMessageBoxUI(ctx, base);
  REXCVAR_SET(headless, old_headless);
}
