# ss2-rtx-remix

System Shock 2: 25th Anniversary Remaster, path traced by NVIDIA's RTX Remix runtime, on
Linux through Proton.

The remaster is a Direct3D 11 game, and Remix is built for Direct3D 9 games. This project
is an adapter (`ss2rtx`, a `dxgi.dll` proxy) that sits in the game's process, reads the
scene out of the game's D3D11 draw calls, hands it to the Remix runtime through the Remix
C API, and puts Remix's finished image back into the game's own frame before the game
draws its weapon, HUD and menus on top.

**Work in progress.** Every mission loads and renders path traced, and it is playable, but
nobody has played the game through this way. See "What works" and "What does not" below.

Not affiliated with Nightdive Studios or NVIDIA. You need your own copy of the game and
your own download of the Remix runtime; neither is in this repository.

## What works

- The level, objects, characters, textures and camera of all 23 missions, from the game's
  own draw calls. No game files are modified.
- The level's lights, read from the mission file the game has loaded (recognised by the
  level's geometry), including spotlights; flashing, flickering and switched lights follow
  the engine's own per-frame values; muzzle flashes and other moving lights.
- Brightness that matches the engine's: the same views rendered both ways come out within
  a few percent of each other in mean brightness.
- Screens, lit signs, glow layers, decals, see-through surfaces, the sky box through
  windows.
- The game's HUD, weapon, menus, saves and input, untouched.
- An in-game settings panel (F11): path tracing on/off, upscaler and DLSS mode, ray
  reconstruction, DLSS neural rendering, indirect light mode, bounces, exposure, bloom and
  the adapter's own light levels. Scroll Lock switches between path traced and the
  engine's renderer at any time.
- A material override table (`ss2rtx/materials.txt`, reloaded live) for roughness,
  metalness, normal and height maps per texture.
- Installer and uninstaller with a manifest and backups; a launcher for a private test
  copy that always closes what it started; scripted test runs, a screenshot regression
  check, a 23-mission sweep and a tool that renders the same views both ways for
  comparison.

Measured on an RTX PRO 6000 under Proton, Remix render plus hand-off per frame: 3.2 to
4.6 ms at 1280x720 (DLSS at full resolution), about 7 ms at 1080p, about 8 ms at 4K with
DLSS Performance. The game stays at its 120 fps cap at 720p and 1080p.

## What does not

- Particles and other sprites the engine draws inside its world pass are missing from
  the path-traced image.
- Water is the engine's flat textured surface: no refraction.
- Lights fall off with the square of the distance, the engine's with the distance. They
  are matched at five game units and lights close to a wall are moved a little way out
  from it, so nearby surfaces match; far ones are dimmer than the engine's and rely on
  bounced light.
- Materials have one default roughness unless you write overrides. No remastered content
  is included.
- Remix's replacement mods (the RTX Remix Toolkit workflow) do not apply to content
  submitted through the API in runtime 1.6.1, so the Toolkit cannot be used to author
  for this. The override table is the authoring route.
- Changing the resolution in the game's options while path tracing is untested. Thin
  glass is an untested option (`Glass=1`).
- Linux and Proton only so far. The adapter is ordinary Win32 code and may well work on
  Windows; it has not been tried.

## How it works

```
SystemShock2Remastered.exe (D3D11)
 └─ dxgi.dll = ss2rtx adapter
     ├─ hooks the D3D11 device, context and swap chain
     ├─ names the game's shaders by hashing their bytecode against the game's shader archive
     ├─ level pass  -> one mesh for the level, lights from the mission file
     ├─ object pass -> meshes, transforms, bones, textures, per-draw material constants
     ├─ Remix C API -> meshes, materials, lights, camera, instances; Remix path traces
     └─ at the engine's first post-process draw: Remix's image is copied into the game's
        render target (shared texture between Remix's Vulkan device and the game's D3D11)
```

Source layout:

| Path | What |
|---|---|
| `adapter/src/dxgi_proxy.cpp` | the proxy DLL, swap chain hooks, keys |
| `adapter/src/scene.cpp` | scene capture from the draw stream: level, objects, lights, sky |
| `adapter/src/remix_link.cpp` | everything said to Remix; the hand-off; the settings table |
| `adapter/src/mission.cpp`, `kpf.cpp` | reading the game's mission files from its archives |
| `adapter/src/menu.cpp` | the F11 panel |
| `adapter/src/census.cpp` | dumps one frame's draw stream for analysis |
| `tests/` | stand-alone probes: Remix under Proton, readback, the D3D11 hand-off |
| `build.py`, `install.py`, `launch.py`, `play.py`, `regress.py` | build, install, test launcher, Steam launch wrapper, regression check |
| `tools/` | mission sweep, start-anywhere, side-by-side survey, draw-stream census reader |

## Building and running

You need:

- The game (Steam app 866570) with its renderer set to Direct3D 11.
- Proton and an NVIDIA RTX GPU with a Vulkan ray tracing driver.
- NVIDIA's RTX Remix runtime 1.6.1 for x64 games
  (`rtx-remix-for-x64-games-release-remix-1.6.1.zip`,
  sha256 `7dd1d44492a609050e44b0e3581cec31135d1ed88b4a91123b4faae91955716a`), unpacked
  to `runtime/remix-1.6.1-x64/`.
- The matching API header `remix_c.h` (dxvk-remix tag `remix-1.6.1`,
  `public/include/remix/`) in `runtime/sdk-1.6.1/include/remix/`.
- llvm-mingw (built with 20261104, UCRT) at `toolchain/llvm-mingw/`. No Windows SDK.
- Python 3 with Pillow for the test tools.

Then:

    python3 build.py adapter                 # builds adapter/build/dxgi.dll
    python3 install.py install --steam       # into the Steam game folder, with backups
    python3 install.py uninstall --steam     # puts everything back

and set the game's Steam launch options to

    /usr/bin/env -u LD_LIBRARY_PATH /usr/bin/python3 /path/to/ss2-rtx-remix/play.py -- %command%

`USAGE.md` has the keys, every setting, material overrides, logs and the developer
commands (private test copy, scripted runs, sweep, survey).

`setup_game.py` and `gpu_session.py` carry assumptions from the machine this was
developed on (a second project folder to copy a game profile from; a GPU service to
pause, named in an untracked `local.json`). Read them before use.

## Running Remix under Proton: what is not optional

Found the hard way; the adapter and the configs it writes follow all of these.

1. **Runtime folder on the DLL search path.** The release's `d3d9.dll` is a shim that
   loads `d3d9_x64.dll` and about forty dependencies by name. Call `SetDllDirectoryW` on
   the runtime folder before initialising the API.
2. **Frame generation off** (`rtx.dlssPreset = 2`, `rtx.dlfg.enable = False`). With DLSS
   frame generation available Remix uses a separate present queue, and under Wine the main
   thread deadlocks on the third present.
3. **Remix's GUI input method off** (`rtx.useNewGuiInputMethod = False`). Otherwise it
   registers raw input for the whole process and the game loses keyboard and mouse.
4. **All texture mips** (`rtx.texturemanager.neverDowngradeTextures = True`). Otherwise
   texture streaming leaves API-supplied textures at a small mip and everything is blurred.
5. **Own the D3D9 device** (`dxvk_CreateD3D9`, `CreateDeviceEx`, `dxvk_RegisterD3D9Device`)
   and present with `PresentEx`.
6. **Hand-off direction: D3D11 exports, Remix imports.** D3D11 creates a shared
   B8G8R8A8 texture and Remix's D3D9 `CreateTexture` imports its handle. The other
   direction fails on Proton 11.
7. **Exit with `TerminateProcess`.** Remix leaves threads behind after shutdown.
8. **Crash reporting.** The runtime contains consent-gated crash reporting. Nothing here
   consents, and the launchers point its upload address at a dead local port. Do not set
   `RTX_AUTOMATION_DISABLE_BLOCKING_DIALOG_BOXES=1`: that grants consent.

Other findings worth knowing if you build on the Remix API:

- **G1b: replacement mods do not apply to API-submitted content** in 1.6.1. A hand-written
  mod replacing eleven of our materials loaded and changed nothing; external draws bypass
  the replacement lookup.
- **A mesh is known by its hash.** Creating a replacement mesh with the same hash and
  then destroying the old one destroys the new one too.
- **DLSS neural rendering is masked per material** (the `dlssControlMask*` fields of the
  opaque material record, API 0.6.5). Left at zero, the model runs and changes nothing.
- **Alpha-tested "decals" are only decals if the material blends**, and emissive blended
  layers go to the unordered acceleration structure.
- Remix's default auto-exposure and local tone mapper brighten dark rooms considerably.
  For a game's own brightness: fixed exposure, tone mapping off.

## Licence

MIT, see `LICENSE`. That covers the code in this repository only. System Shock 2 and its
assets belong to their owners; RTX Remix and DLSS are NVIDIA's and come under NVIDIA's
licences.
