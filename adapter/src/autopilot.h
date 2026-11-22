#pragma once
#include <windows.h>

namespace ss2rtx {

// Call once per presented frame. `seconds` is the time since the first Present.
void autopilot_tick(HWND hwnd, double seconds);

}  // namespace ss2rtx
