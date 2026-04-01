#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint> // For uintptr_t
#include <rex/cvar.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/logging.h>
#include <rex/graphics/flags.h>
#include "globals.h"

auto frameTime = std::chrono::system_clock::now();
int frame = 0;


REXCVAR_DEFINE_INT32(target_fps, 30, "Lollipop/Performance", "Target frame rate cap. 30 = original, 60 = unlocked")
.range(30, 60)
.validator([](std::string_view v) {
    return v == "30" || v == "60";
    });

void fps(PPCRegister& r11) {
    r11.u32 = (REXCVAR_GET(target_fps) == 60) ? 1u : 2u;
}

void counter() {
    frame++;
    auto Time = std::chrono::system_clock::now();
    std::chrono::duration<double, std::milli> delta = Time - frameTime;
    frameTime = Time;
    double fpsfromMS = 1000 / delta.count();
    if (frame >= 60) {
        frame = 0;
        fpsCount = fpsfromMS;
    }
}
