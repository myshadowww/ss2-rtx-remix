// The Remix side: owns the Remix runtime inside the game process and feeds it the scene.
#pragma once
#include <string>
#include <windows.h>
#include <d3d11.h>

namespace ss2rtx {

// Tells the link how large the game's frame is. Call when the swap chain is created or resized.
void remix_link_set_output_size(unsigned width, unsigned height);

// Call once per presented frame on the game's render thread. Starts Remix on first use
// once the scene has a level (and only if SS2RTX_REMIX_DIR names the runtime folder),
// then submits the scene and lets Remix render one frame.
void remix_link_frame(double seconds);

struct ShaderRecord;

// Call before every draw on the game's immediate context. At the end of the engine's
// opaque world pass this renders the Remix frame and copies it over the engine's world
// image. Returns true if the game's draw should be skipped.
bool remix_link_before_draw(ID3D11DeviceContext* ctx, const ShaderRecord* vs, const ShaderRecord* ps);

// Turns the path-traced image on or off; off shows the engine's own rendering.
void remix_link_set_enabled(bool enabled);
bool remix_link_enabled();

// True once the Remix runtime has been loaded into the process.
bool remix_link_started();

// Switches the emissive glow layers on or off while running (a diagnostic for shimmer).
void remix_link_toggle_glow();

// "Something looks wrong right now": writes the scene's state to the log and saves the
// next path-traced frame as remix-N.bmp.
void remix_link_mark(double seconds);

// The settings the in-game panel shows (menu.cpp draws them and steps them).
int remix_settings_count();
// Fills in row i. Returns true when the row is a heading, which has no value.
bool remix_settings_row(int i, std::wstring* label, std::wstring* value);
// Moves row i's value one step down (-1) or up (+1), applies it and stores it.
void remix_settings_step(int i, int direction);
// One line about what the renderer is doing, for the foot of the panel.
std::wstring remix_settings_status();

}  // namespace ss2rtx
