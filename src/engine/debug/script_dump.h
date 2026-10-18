#pragma once

#include <cstdint>
#include <string>

namespace cherry::engine::debug {

// Writes the script class and value dumps their cvars ask for; called once per game-thread engine tick.
void TickScriptDump();

// The value of property stored at address, as the dumps print it.
std::string FormatScriptValue(uint32_t address, uint32_t property);

}  // namespace cherry::engine::debug
