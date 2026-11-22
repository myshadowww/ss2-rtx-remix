// The in-game settings panel: a key (F11 unless MenuKey says otherwise) opens it over the
// game's picture, the arrow keys move through the rows and change them.
#pragma once
#include <windows.h>
#include <dxgi.h>

namespace ss2rtx {

// Call once the game's window is known. Takes the panel's keys out of the game's hands
// while they are meant for the panel.
void menu_install(HWND game_window);

// Call before every Present: acts on the keys pressed and draws the panel when it is open.
void menu_present(IDXGISwapChain* swapchain);

}  // namespace ss2rtx
