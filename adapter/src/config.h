// Adapter settings. Each setting is read from the environment variable SS2RTX_<NAME>
// first (used by the test launchers), then from ss2rtx\ss2rtx.ini in the game folder:
//
//   [ss2rtx]
//   RemixDir=ss2rtx\remix     ; folder holding the Remix runtime's d3d9.dll
//   Composite=1               ; put the path-traced image into the game's frame
//   LightScale=40
#pragma once
#include <string>

namespace ss2rtx {

std::wstring config_text(const wchar_t* name);          // empty if unset
bool config_flag(const wchar_t* name, bool fallback = false);
double config_number(const wchar_t* name, double fallback);

// Stores a setting in ss2rtx\ss2rtx.ini (the in-game settings panel does this). Does
// nothing when SS2RTX_NOSAVE=1, which the test launcher sets so test runs stay the same.
void config_set(const wchar_t* name, const std::wstring& value);

}  // namespace ss2rtx
