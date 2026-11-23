# ss2rtx — using it

System Shock 2: 25th Anniversary Remaster rendered by RTX Remix, on Linux through Proton.
This is a work in progress; `README.md` says what works and what does not.

## Try it in the private copy (touches nothing of yours)

    cd /path/to/ss2-rtx-remix
    python3 gpu_session.py begin          # only if local.json names a service that fills the GPU: pauses it
    python3 launch.py --composite --game-arg=-skipmovies
    python3 gpu_session.py end            # restarts it (only if begin stopped it)

The game runs from `remix/game` with its own profile and saves under `remix/prefix`
(copies of your saves are also in `remix/saves`). Load Game has the quick-save in Ramsey
Center and two slots at the start of MedSci. Closing the game closes everything the
launcher started.

In game:
- **F11** opens the settings panel. Up/Down choose a row, Left/Right change it, F11 or
  Esc closes it; the game does not see those keys while it is open. Changes apply at once
  and are stored in `ss2rtx/ss2rtx.ini`. It has: path tracing on/off; upscaler and DLSS
  mode; ray reconstruction; neural rendering with its model and strengths; how indirect
  light is computed, bounces, RTXDI; the look (engine's brightness or Remix's own),
  exposure, bloom, volumetric light, motion blur; and the adapter's light and emissive
  levels and its scene switches. The line at the bottom shows the path tracing cost per
  frame, so you can see what a setting costs. F11 was the game's quick-use slot 11;
  `MenuKey` moves the panel to another key (virtual-key code).
- **Scroll Lock** switches between the path-traced image and the engine's own rendering.
- **Pause/Break** switches the glow layers (lamp glows, the writing on screens).
- `--split` instead of `--composite` shows the engine on the left half and the
  path-traced image on the right half of the same frame.

- **Insert** marks a glitch. When something looks wrong, press it while it is on screen:
  the adapter writes the scene's state to the log and saves that frame's draw stream
  (`census-N.jsonl`) and path-traced image (`remix-N.bmp`) into the game's `ss2rtx`
  folder. That is what is needed to work out a fault that a still screenshot cannot show.
  Expect one slow frame.

The keys can be changed (`ToggleKey`, `GlowKey`, `MarkKey`). They are not function keys
because the game uses every one of those for its quick-use slots.

## Install into the real Steam game

    python3 install.py install --steam        # finds the game through Steam's library list
    python3 install.py status --steam
    python3 install.py uninstall --steam      # puts back whatever dxgi.dll and rtx.conf were there
    python3 install.py install --target <folder>   # any other copy of the game

What is missing is fetched first: NVIDIA's RTX Remix runtime 1.6.1 from NVIDIA's release
page (557 MB, into `runtime/`, checked against a known SHA-256), and the adapter from this
project's releases unless you have built it (`python3 build.py adapter`).

It replaces a `dxgi.dll` that is already in the game folder (another mod's hook, such as
the SS2GraphicsHook upscaler mod), so the two cannot be active together; uninstall puts
the other one back.

Then set the game's Steam launch options (Properties > General) to the line the installer
prints (`python3 install.py launch-options` prints it again):

    WINEDLLOVERRIDES="dxgi=n,b" RTX_SENTRY_TEST_DSN="http://none@127.0.0.1:9/0" %command%

The first part makes Proton load the adapter in place of its own `dxgi.dll`. The second
points Remix's crash reporter at a dead local address; it is consent-gated and nothing
here consents, and this makes sure. The game's renderer must be Direct3D 11.

With several GPUs, or with a service that normally fills the GPU's memory, use the
wrapper instead:

    /usr/bin/env -u LD_LIBRARY_PATH /usr/bin/python3 /path/to/ss2-rtx-remix/play.py -- %command%

It reads `local.json` next to it (not in the repository; every key optional):
`{"gpu": 0, "pause_service": "some-unit.service"}` pins the game to that `nvidia-smi` GPU
index, and stops that `systemd --user` service while the game runs and starts it again
afterwards if it was running.

Run the install again after every rebuild of the adapter or update of this repository;
your `ss2rtx.ini` (what the F11 panel stored) and `materials.txt` are kept, `rtx.conf` is
rewritten.

The installer was tested on a scratch folder: fetch and unpack of the runtime, install
over an existing `dxgi.dll`, then uninstall, which leaves the folder as it was.

## Settings

`<game>/ss2rtx/ss2rtx.ini` (created by the installer; `SS2RTX_<NAME>` environment
variables override it):

| Setting | Default | Meaning |
|---|---|---|
| `RemixDir` | `ss2rtx\remix` | folder with the Remix runtime |
| `Composite` | 1 | put the path-traced image into the game's frame |
| `Split` | 0 | left half engine, right half path-traced |
| `LightScale` | 40 | engine light colour to Remix radiance |
| `LightRadius` | 0.2 | size of the light spheres, in game units |
| `Emissive` | 0.4 | brightness of full-bright object parts (lamp faces); 0.32 already shows as full white |
| `ScreenEmissive` | 0.4 | brightness of full-bright level surfaces and glow layers (screens, lit signs); these light the room around them |
| `EmissiveLight` | 0.15 | how much of a lamp face's or screen's brightness it also casts as light; the engine's cast none, 1 lets them light their rooms fully |
| `AmbientFloor` | 1 | surfaces give off the engine's ambient level themselves (the darkest its lightmaps go), so a corner no lamp reaches is dim as in the engine and not black; 0: off |
| `LiveLights` | 1 | flashing, flickering and switched lights follow the engine's own value each frame (or replay their rhythm from the mission file where that is not to be had); 0: every light steady at its table value |
| `FitLights` | 1 | fit each light's strength to the level, where a plain match at five units leaves corridors too bright and big rooms too dark |
| `FitLightmaps` | 1 | do that fit against the engine's own baked lightmaps from the mission file (0: against an assumed falloff rule only) |
| `FitTexels` | 1 | ...texel by texel, each where it lies on its polygon, with a shadow ray from every point (0: one mean value per polygon) |
| `FitRule` | 1 | what the fit matches: 1 every point's own ratio, weighted by what the lamp sends it (the points nearest a lamp weigh most, far walls err dark); 0 the light that shows on screen over each lamp's whole area (brighter: more places came out too bright in the walk) |
| `FitHeadroom` | 2 | a texel the engine baked to full white counts against its lamps once they send it this many times white (0: never; brighter lamps and rooms) |
| `LinearLightColour` | 1 | convert light colours to linear light so coloured lights tint as strongly as the engine's |
| `StandOff` | 1 | move lights that sit close to a wall a little way out from it, so the wall gets a soft glow as in the engine, not a small bright disc |
| `Sprites` | 1 | put the engine's particles, sparks and coronas into the path-traced scene (they are drawn before the point where the path-traced image replaces the engine's, so otherwise they are lost) |
| `Water` | 1 | water surfaces, which the mission file marks: 0 as the engine textures them, 1 the same but mirror-smooth so they reflect, 2 a see-through tinted skin, 3 a boundary that bends light |
| `SpriteVertexColour` | 1 | particles keep each vertex's colour and fade, through the engine's own blend; 0: one averaged colour a batch |
| `Look` | `engine` | `engine`: the engine's brightness, no tone mapping; `engine-tonemapped`: the same brightness through Remix's local tone mapper; `remix`: Remix's own auto exposure and tone mapping; `custom`: whatever the next three say |
| `ToneMapping` | `off` | `off`, `local` (Remix's local tone mapper, filmic) or `global` (Remix's global curve, which follows the view) |
| `AutoExposure` | 0 | let Remix adapt brightness to what is on screen |
| `Exposure` | 1.65 | in stops; with auto exposure off 1.65 is the engine's brightness under every tone mapping, with it on 0 is Remix's default |
| `Sky` | 1 | send the engine's sky box to the path tracer, so windows show the stars and the Earth |
| `FollowEngineLights` | 0 | also take each light's brightness from the engine's per-object lists; off because those values are what reaches one object (zero in its shadow), which made lights switch with the view |
| `KeepObjects` | 1 | keep still objects in the path-traced scene after they leave the view (the engine stops drawing them); 0: only what the engine draws |
| `MarkKey` | 45 | virtual-key code of the "mark a glitch" key (45 is Insert) |
| `SeeThrough` | 1 | show the engine's see-through surfaces (stains, floor sheen, panes) with its own blend; 0 leaves them out |
| `NeuralRendering` | 0 | Remix's DLSS neural rendering (the DLSS 5 rows of the F11 panel: model, intensity, structure, tone); about 1.6 ms a frame at 720p and 4 ms at 4K |
| `GlowLayers` | 1 | the light the engine adds on top of surfaces (lamp glows, writing on screens) |
| `GlossRoughness` | 0.3 | roughness of surfaces the engine fakes a reflection on (polished panels) |
| `Dlss` | `auto` | `auto`: full resolution up to 1200 lines of output, Remix's own choice above; `conf`: whatever `rtx.conf` says; `0`–`5`: force a mode (0 ultra performance, 3 quality, 5 full resolution) |
| `Glass` | 0 | make upright see-through surfaces thin glass instead (flat-lying ones are then left out); no pane has been found to look at with it |
| `ToggleKey` | 145 | virtual-key code of the path tracing switch (145 is Scroll Lock) |
| `GlowKey` | 19 | virtual-key code of the glow layer switch (19 is Pause) |
| `TwoSided` | 0 | ignore the engine's back-face culling (debugging) |

`<game>/rtx.conf` holds Remix's own options. These must stay as written:
`rtx.dlssPreset = 2` and `rtx.dlfg.enable = False` (frame generation deadlocks under
Proton), `rtx.useNewGuiInputMethod = False` (otherwise the game loses keyboard and mouse)
and `rtx.texturemanager.neverDowngradeTextures = True` (otherwise every texture is
blurred). The three `rtx.postfx` lines turn off Remix's motion blur, chromatic aberration
and vignette, and `rtx.bloom.enable = False` its bloom (the game has a bloom setting of
its own, applied to the finished frame); those are a matter of taste.

The four tone-mapping lines (`rtx.tonemappingMode`, `rtx.autoExposure.enabled`,
`rtx.tonemap.tonemappingEnabled`, `rtx.tonemap.exposureBias = 1.65`) give a fixed exposure
with no tone curve, which puts the picture at the engine's own brightness: dark rooms
stay dark. Remove them for Remix's look (auto-exposure and a local tone mapper), which
brightens dark rooms and flattens contrast. `LightScale` and the emissive settings were
set against the fixed exposure.

If the frame rate drops at 1080p, set `Dlss=3`: about 4 ms a frame instead of 7.

## Tuning materials

Remix's own replacement mods do not apply to what the adapter submits (tested, see
the notes under G1b in the README), so materials are tuned through `<game>/ss2rtx/materials.txt`:

    # one line per texture; the hash is the texture's name in ss2rtx_cache\textures\
    63BB90387AE74720 roughness=0.35 metallic=1 normal=pbr\floor_n.dds
    C440D8122A08FB88 emissive=self emissive_intensity=4
    default roughness=0.75 metallic=0

Keys: `roughness`, `metallic`, `emissive_intensity`, `displace`, `albedo`, `normal`,
`roughness_tex`, `metallic_tex`, `height`, `emissive` (a `.dds` path relative to the file,
or `self`). Textures must be `.dds`. The file is re-read when it changes, so you can edit
it with the game running; expect one slow frame while everything is rebuilt.
`ss2rtx_cache/textures_seen.txt` lists every texture the adapter has met, and the
`.dds` files next to it are the textures themselves.

A starter set, made by a script from the textures themselves:

    python3 tools/author.py --steam      # or no option: the private test copy
    python3 tools/author.py --steam --remove

For every texture the adapter has met so far it works out a normal map (the relief the
texture's light and dark suggest) and a roughness map (grey, pale surfaces smoother,
coloured and dark ones matt; raised parts smoother than seams), writes them to
`ss2rtx/authored/` and adds a marked block of lines to `materials.txt`. Screens, signs,
cut-outs and small textures are left alone. Lines of your own after the block win, and a
line replaces the whole entry for its texture. Run it again after visiting new decks. It
needs numpy and Pillow. It is a guess from colour, not art: where a texture has painted
shadows the relief is wrong, and you can delete that line. The maps are in the formats
Remix reads natively: normals as two-channel octahedral BC5, roughness as BC4.

## Logs

- `ss2rtx/ss2rtx.log` in the game folder (or `run/game/ss2rtx.log` under the launcher):
  what the adapter did, including a line every ten seconds with instance, light and
  timing counts. The session before is kept as `ss2rtx-prev.log`.
- `remix-dxvk.log`: the Remix runtime's own log.

## Developer commands

    python3 build.py adapter                               # rebuild the adapter
    python3 launch.py --seconds 45 --scenario ramsey --split --screenshot-at 40
    python3 launch.py --seconds 44 --scenario medsci --composite --screenshot-at 33 --screenshot-at 40 --keys "36:rtx_off"
    python3 launch.py --scenario ramsey --census-at 34     # dump one frame's draw stream
    python3 tools/census.py run/game/census-0.jsonl
    python3 runtest.py t03_handoff r1 3840x2160 100 --seconds 60   # Phase 0 hand-off probe

    python3 tools/tour.py medsci2                          # make New Game start in any deck (private copy only)
    python3 launch.py --scenario tour --composite          # ... and start it there
    python3 tools/tour.py off
    python3 tools/tour.py medsci2 --at=-12,-190,2,0        # ...with the player put at that spot, facing 0 degrees
    python3 tools/survey.py medsci2 -12,-190,2,0 icu       # same four views path traced and engine, into run/survey/
    python3 tools/author.py                                # normal and roughness maps for the private copy's textures (see Tuning materials)
    python3 tools/steam_game_running.py && echo "the Steam copy is running"
    python3 tools/walk.py                                  # three places in each of the 23 decks, both renderers, into run/walk/ (75 minutes)
    python3 tools/walk.py ops1 --spots 5                   # one deck, more places
    python3 tools/sweep.py                                 # all 23 missions path-traced, results in run/sweep/

    python3 regress.py --update                            # store screenshots as the baseline
    python3 regress.py                                     # rerun and compare with it

Every launcher closes all windows and processes it started, on any exit path. A scripted
run (`--scenario` or `--keys`) ignores the real keyboard and mouse so that using the
desktop cannot disturb it; add `--live-input` to drive it by hand as well. Do not run
anything heavy on the disk at the same time: the menu keys are timed.

## Limits worth knowing

- The path-traced image goes in at the engine's first post-process draw of the frame
  (ambient occlusion, bloom, anti-aliasing or colour correction). With every one of those
  turned off in the game's settings there is nothing to hook, and the engine's image shows.
- Particles and sparks are in the path-traced scene as glowing or blended quads.
  `Sprites=0` leaves them out.
- After path tracing is switched back on (Scroll Lock) the engine's picture stays up for
  a second and a half while Remix's lighting settles.
- The sky is the engine's own box and textures, shown at the engine's brightness; it
  lights nothing to speak of. `Sky=0` in `ss2rtx.ini` leaves it out. Water is the
  engine's texture made mirror-smooth by default; `Water` can make it a see-through skin
  or a boundary that bends light.
- Path-traced rooms are lit by bounced light as well, so walls near a lit floor are
  brighter than the engine's; screens and lamps light what is around them.
- Flashing, flickering and switched lights take their brightness from the engine every
  frame, when the mission file's cells can be tied to the level (the log says "N animated
  lights take their brightness from the engine"). A light that reaches no level surface
  replays its rhythm from the file instead.
- Lights fall off with the square of the distance (the engine's: with the distance). Each
  light's strength is fitted to what the engine baked into its lightmaps, texel by texel
  and less what bounced light will add, and lights close to a wall are moved a little way
  out from it (two units at most) so the wall gets a glow, not a small bright disc.
  Measured against the engine at 69 places over all 23 decks: 46 of 68 usable
  views within 25% of the engine's mean level, 59 within 50% (median 1.06; a surface close
  to a lamp tends to be darker than the engine's, a far one brighter). Colours are close; the
  engine's are a little stronger. F11, exposure, changes the overall level. The fit takes
  up to three quarters of a second when a deck loads.
- Surfaces the engine paints a canned reflection on are polished or mirror surfaces and
  reflect the room as it is. The engine's view-dependent "incidence" maps are applied as
  their average only.
- The first time a deck is visited the adapter knows only the polygons it has been shown;
  the rest stand in as plain grey until seen (they are remembered in `ss2rtx_cache` from
  then on). This build starts those files afresh.
- `Skip=<letters>` in `ss2rtx.ini` leaves parts of the scene out of the path-traced image
  (o objects, l lights, c characters): for finding where something on screen comes from.
- Every mission loads and renders, and a script has looked at three places in each
  (`tools/walk.py`). Nobody has played a level through this way.
