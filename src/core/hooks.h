// The midasm hooks config/hooks/*.toml names; codegen declares and calls them from the recompiled code with these signatures.
#pragma once

#include "re_cherry_init.h"

// config/hooks/display.toml, src/engine/widescreen.cpp
void CherryBackBufferWidth(PPCRegister& r11);
bool CherryKeepsWideScreenBudget(PPCRegister& r11);
