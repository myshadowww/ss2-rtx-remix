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

Not affiliated with Nightdive Studios or NVIDIA. You need your own copy of the game.
NVIDIA's Remix runtime is not in this repository; the installer fetches it from NVIDIA's
own release page.

## Quick start

You need the game on Steam (app 866570), Proton, an NVIDIA RTX GPU with a current driver,
and Python 3.

1. Get this repository and run the installer. It finds the game through Steam, fetches
   the adapter and NVIDIA's RTX Remix runtime 1.6.1 (557 MB, checked against a known
   SHA-256), and backs up every file it replaces:

       git clone https://github.com/myshadowww/ss2-rtx-remix
       cd ss2-rtx-remix
       python3 install.py install --steam

2. In Steam, open the game's Properties > General and set the launch options to the line
   the installer printed:

       WINEDLLOVERRIDES="dxgi=n,b" RTX_SENTRY_TEST_DSN="http://none@127.0.0.1:9/0" %command%

3. Start the game with its renderer set to Direct3D 11 (Options > Video). F11 opens the
   settings panel; Scroll Lock switches between path traced and the engine's renderer.

`python3 install.py uninstall --steam` puts everything back; then clear the launch
options. `USAGE.md` has the keys, every setting, material overrides and logs.

## What works

- The level, objects, characters, textures and camera of all 23 missions, from the game's
  own draw calls. No game files are modified.
- The level's lights, read from the mission file the game has loaded (recognised by the
  level's geometry), including spotlights; flashing, flickering and switched lights follow
  the engine's own per-frame values; muzzle flashes and other moving lights.
- Brightness and light colour close to the engine's: each light's strength is fitted to
  the lightmaps the engine baked (read from the mission file texel by texel, each texel
  placed on its polygon, with a shadow ray to every light), in linear light and less
  what bounced light adds, channel by channel, with the engine's ambient level as a floor.
  At 69 places over all 23 decks (three a deck, picked by a script), 46 of the 68
  usable views are within 25% of the engine's mean level and 59 within 50%; the rest are
  mostly on the dark side by choice.
- Particles and sparks with the engine's own blend and per-vertex colour; water surfaces
  (mirror-smooth, a see-through skin, or a boundary that bends light).
- Screens, lit signs, glow layers, decals, see-through surfaces, the sky box through
  windows. Textures the engine slides across a surface (the plasma floors of the last
  level, two layers drifting over each other) move as they do in the engine. Surfaces the engine paints a canned reflection on (its cube-map passes) are
  polished or mirror surfaces here and reflect the room.
- The game's HUD, weapon, menus, saves and input, untouched.
- An in-game settings panel (F11): path tracing on/off, upscaler and DLSS mode, ray
  reconstruction, DLSS neural rendering, indirect light mode, bounces, the look (the
  engine's brightness, the same through Remix's tone mapper, or Remix's own, with tone
  mapping, auto exposure and exposure as separate rows), bloom and the adapter's own
  light levels. Changing the resolution in the game's options works while path tracing. Scroll Lock switches between path traced and the
  engine's renderer at any time.
- A material override table (`ss2rtx/materials.txt`, reloaded live) for roughness,
  metalness, normal and height maps per texture; values chosen by hand for the 46
  surfaces that cover most of the decks (`materials/handmade.txt`: which are steel,
  enamel, flesh, carpet, rubber); and a script (`tools/author.py`) that merges those with
  a normal map and a roughness map worked out from each of the game's textures on your
  machine.
- Installer and uninstaller with a manifest and backups; a launcher for a private test
  copy that always closes what it started; scripted test runs, a screenshot regression
  check, a 23-mission sweep and a tool that renders the same views both ways for
  comparison.

Measured on an RTX PRO 6000 under Proton, Remix render plus hand-off per frame: 3.2 to
4.6 ms at 1280x720 (DLSS at full resolution), about 7 ms at 1080p, about 8 ms at 4K with
DLSS Performance. The game stays at its 120 fps cap at 720p and 1080p.

## What does not

- Lighting is fitted, not identical: lights fall off with the square of the distance
  and the engine's with the distance, so surfaces near a light are brighter than the
  engine's and far ones darker even when the room as a whole matches. Coloured light is a
  little weaker than the engine's.
- The engine's view-dependent "incidence" maps (a part's colour and opacity by viewing
  angle and distance) are applied as their average only.
- Where the engine adds a faint layer as displayed, the same layer added as light is far
  brighter (a fiftieth of opacity shows as a sixth). Such layers are left out or cut; the
  thin grid lines over a lit plasma floor are among what is lost.
- Materials are a start: roughness and metalness by hand for 46 surfaces, relief and the
  rest a guess from each texture's colour. None is installed unless you run the script.
- Remix's replacement mods (the RTX Remix Toolkit workflow) do not apply to content
  submitted through the API in runtime 1.6.1, so the Toolkit cannot be used to author
  for this. The override table is the authoring route.
- Linux and Proton only so far. The adapter is ordinary Win32 code and may well work on
  Windows; it has not been tried.

## Backlog

- Playing the game through path traced, deck by deck, to find the engine's remaining
  special cases.
- Materials made by hand for the surfaces that matter most (the generated set is a
  starting point).

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
| `build.py`, `install.py`, `launch.py`, `play.py`, `regress.py` | build, install, test launcher, optional Steam launch wrapper, regression check |
| `tools/` | mission sweep, start-anywhere, side-by-side survey, a walk through every deck in both renderers, draw-stream census reader, material starter set |

## Building it yourself

    python3 build.py adapter                 # builds adapter/build/dxgi.dll
    python3 install.py install --steam       # installs your build instead of the released one

The first build fetches the compiler, llvm-mingw 20261104 (84 MB, checked against a known
SHA-256), into `toolchain/`. Nothing else is needed: no Windows SDK, and the Remix API
header (`remix_c.h`, MIT, from dxvk-remix tag `remix-1.6.1`) is in `adapter/third_party/`.
The test tools need Pillow.

`USAGE.md` has the developer commands: a private test copy of the game, scripted runs,
the regression check, the sweep, survey and walk.

`play.py` is an optional launch wrapper for a machine with several GPUs or with a service
that fills the GPU's memory; `setup_game.py` and `gpu_session.py` carry assumptions from
the machine this was developed on (a second project folder to copy a game profile from).
Read them before use.

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
- **A spotlight's `coneSoftness` is a span of the cosine of the angle**, not of the angle
  (the shader does a smooth step from the cone's cosine to that plus the softness).
- **A mesh is known by its hash.** Creating a replacement mesh with the same hash and
  then destroying the old one destroys the new one too.
- **DLSS neural rendering is masked per material** (the `dlssControlMask*` fields of the
  opaque material record, API 0.6.5). Left at zero, the model runs and changes nothing.
- **The engine's reflection passes leave a stale texture bound.** A KEX Dark material
  with `CubeMap` set samples the cube map (slot 4) by the mirrored view direction; slot 0
  still holds whatever the draw before left there. Reading it as the surface's texture
  gave lit walls that were not there, and "texture-swapping" polygons that were not.
- **A black non-metal still shines.** A surface the game never shades (a screen, the
  black between a grid's lines) must give back no light at all, and albedo zero is not
  enough: a dielectric returns a twenty-fifth as sheen, which over a room of such walls
  was a wash of the lamps' colour. Black and metallic is truly dark.
- **Alpha-tested "decals" are only decals if the material blends**, and emissive blended
  layers go to the unordered acceleration structure.
- Remix's default auto-exposure and local tone mapper brighten dark rooms considerably.
  For a game's own brightness: fixed exposure, tone mapping off.

## Licence

MIT, see `LICENSE`. That covers the code in this repository only. System Shock 2 and its
assets belong to their owners; RTX Remix and DLSS are NVIDIA's and come under NVIDIA's
licences, which is why the installer fetches the runtime from NVIDIA instead of this
repository carrying it.

`adapter/third_party/remix/remix_c.h` is NVIDIA's Remix API header (MIT, licence text in
the file). The released `dxgi.dll` is this repository's code built with llvm-mingw and
statically linked with its runtime libraries (libc++, libunwind and compiler-rt under
Apache-2.0 with LLVM exceptions; the mingw-w64 runtime under its own permissive terms).
