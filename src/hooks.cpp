#include <atomic>
#include <cstdint>
#include <bit>
#include <rex/cvar.h>
#include <rex/ppc/types.h>
#include <rex/runtime.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <generated/re_cherry_config.h>

REXCVAR_DEFINE_BOOL(fps, false, "Lollipop/Cheats", "fps");

static std::atomic<uint64_t> g_guest_frame_count{0};

static float read_engine_delta_time() {
    auto* rt = rex::Runtime::instance();
    if (!rt) return 0.0f;
    uint8_t* base = rt->virtual_membase();
    if (!base) return 0.0f;
    uint32_t raw = __builtin_bswap32(*(volatile uint32_t*)(base + 0x83354AF0u));
    return std::bit_cast<float>(raw);
}

rex::ui::FrameStats GetGuestFrameStats() {
    float dt = read_engine_delta_time();
    double fps_val = (dt > 0.001f) ? 1.0 / dt : 0.0;
    return {dt * 1000.0, fps_val, g_guest_frame_count.load(std::memory_order_relaxed)};
}

void fps(PPCRegister& r11) {
    g_guest_frame_count.fetch_add(1, std::memory_order_relaxed);
    if (REXCVAR_GET(fps)) { r11.u32 = 60; }
}

void fps1(PPCRegister& r11) {
    if (REXCVAR_GET(fps)) { r11.u32 = 1; }
}
