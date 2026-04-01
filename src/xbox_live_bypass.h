#pragma once

#include <string>

struct PPCContext;

namespace xbox_live_bypass {

bool IsXboxLivePrompt(const std::string& title, const std::string& text);

}
