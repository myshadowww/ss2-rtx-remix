// Draw-stream census (Phase 1): record everything the game draws in chosen frames.
#pragma once
#include <windows.h>
#include <d3d11.h>

namespace ss2rtx {

// Patches the game's import of D3D11CreateDevice so hooks are in place before the first
// shader is created. Safe to call more than once.
void census_install_import_hook();

// Fallback entry for when the device was created before the import hook was installed.
void census_on_device(ID3D11Device* device, const char* via);

// Call once per presented frame, before the real Present. `seconds` is the time since
// the first Present. Frames to capture come from SS2RTX_CENSUS_AT="44,50" (seconds).
void census_on_present(double seconds);

// Records the whole of the next frame's draw stream to census-N.jsonl (one slow frame).
void census_capture_next_frame();

}  // namespace ss2rtx
