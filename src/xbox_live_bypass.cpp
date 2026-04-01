#include "xbox_live_bypass.h"

#include <cstddef>
#include <cctype>
#include <string>

#include <generated/re_cherry_init.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>

REXCVAR_DEFINE_BOOL(disable_xbox_live_prompt, true, "Lollipop/Patches",
                    "Auto-dismiss Xbox Live connection prompts");

namespace {

std::string ReadGuestUtf16Ascii(uint8_t* base, uint32_t guest_ptr) {
    if (!guest_ptr) {
        return {};
    }

    std::string out;
    out.reserve(128);

    for (size_t i = 0; i < 256; ++i) {
        uint16_t ch = PPC_LOAD_U16(guest_ptr + static_cast<uint32_t>(i * 2));
        if (ch == 0) {
            break;
        }
        out.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
    }

    return out;
}

std::string AsciiLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

}  // namespace

namespace xbox_live_bypass {

bool IsXboxLivePrompt(const std::string& title, const std::string& text) {
    const std::string haystack = AsciiLower(title + "\n" + text);
    return haystack.find("xbox live") != std::string::npos ||
           haystack.find("connect to xbox live") != std::string::npos ||
           haystack.find("connect to xbox") != std::string::npos ||
           haystack.find("sign in to xbox live") != std::string::npos ||
           haystack.find("xboxlive") != std::string::npos;
}

}  // namespace xbox_live_bypass

extern "C" void sub_82BF4288(PPCContext& ctx, uint8_t* membase) {
    uint8_t* base = membase;
    uint32_t ea{};
    const uint32_t saved_sp = ctx.r1.u32;
    const uint32_t title_ptr = ctx.r4.u32;
    const uint32_t text_ptr = ctx.r5.u32;

    ctx.r12.u64 = ctx.lr;
    PPC_STORE_U32(saved_sp - 8, ctx.r12.u32);
    ea = saved_sp - 96;
    PPC_STORE_U32(ea, saved_sp);
    ctx.r1.u32 = ea;

    const uint32_t overlapped_ptr = PPC_LOAD_U32(ctx.r1.u32 + 180);
    PPC_STORE_U32(ctx.r1.u32 + 84, overlapped_ptr);

    if (REXCVAR_GET(disable_xbox_live_prompt)) {
        const std::string title = ReadGuestUtf16Ascii(base, title_ptr);
        const std::string text = ReadGuestUtf16Ascii(base, text_ptr);
        if (xbox_live_bypass::IsXboxLivePrompt(title, text)) {
            const bool old_headless = REXCVAR_GET(headless);
            const uint32_t old_active_button = ctx.r8.u32;
            REXCVAR_SET(headless, true);
            if (ctx.r6.u32 >= 2) {
                ctx.r8.u32 = 1;
            }
            ctx.lr = 0x82BF42A0;
            __imp__XamShowMessageBoxUI(ctx, base);
            ctx.r8.u32 = old_active_button;
            REXCVAR_SET(headless, old_headless);
            ctx.r1.u32 = saved_sp;
            ctx.r12.u64 = PPC_LOAD_U32(ctx.r1.u32 - 8);
            ctx.lr = ctx.r12.u64;
            return;
        }
    }

    ctx.lr = 0x82BF42A0;
    if (auto* rt = rex::Runtime::instance()) {
        __imp__XamShowMessageBoxUI(ctx, rt->virtual_membase());
    }
    ctx.r1.u32 = saved_sp;
    ctx.r12.u64 = PPC_LOAD_U32(ctx.r1.u32 - 8);
    ctx.lr = ctx.r12.u64;
}
