#!/usr/bin/env python3
"""Install or remove the ss2rtx adapter and Remix runtime in a System Shock 2 Remastered folder.

  python3 install.py install --steam                # the Steam installation, found through Steam's library list
  python3 install.py uninstall --steam
  python3 install.py status --steam
  python3 install.py launch-options                 # the line to paste into the game's Steam launch options
  python3 install.py install --target <folder>      # any other copy of the game

What is not there yet is fetched: NVIDIA's RTX Remix runtime 1.6.1 from NVIDIA's own
release page (557 MB, checked against a known SHA-256), and the adapter itself from this
project's releases if it has not been built here (build.py adapter).

Every file that is replaced is first copied to <target>/ss2rtx/backup/, and everything
done is recorded in <target>/ss2rtx/install-manifest.json, which `uninstall` replays in
reverse. Nothing outside the target folder is touched. An existing dxgi.dll (another mod's
hook, say) is replaced; uninstall puts it back. The Steam launch options are not changed
by this script: it prints the line to set.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GAME_FOLDER = 'System Shock 2 Remastered'
RUNTIME = ROOT / 'runtime/remix-1.6.1-x64'
RUNTIME_URL = 'https://github.com/NVIDIAGameWorks/rtx-remix/releases/download/remix-1.6.1/rtx-remix-for-x64-games-release-remix-1.6.1.zip'
RUNTIME_SHA256 = '7dd1d44492a609050e44b0e3581cec31135d1ed88b4a91123b4faae91955716a'
ADAPTER = ROOT / 'adapter/build/dxgi.dll'
ADAPTER_URL = 'https://github.com/myshadowww/ss2-rtx-remix/releases/latest/download/dxgi.dll'
# The game needs our dxgi.dll loaded in place of Wine's, and Remix's crash reporter (which
# is consent-gated and has no consent) pointed at a dead address.
LAUNCH_OPTIONS = 'WINEDLLOVERRIDES="dxgi=n,b" RTX_SENTRY_TEST_DSN="http://none@127.0.0.1:9/0" %command%'


def find_game():
    """The game's folder, looked for in every Steam library on this machine."""
    roots = [Path.home() / '.local/share/Steam', Path.home() / '.steam/steam', Path.home() / '.steam/root',
             Path.home() / '.var/app/com.valvesoftware.Steam/.local/share/Steam']
    libraries = []
    for root in roots:
        listing = root / 'steamapps/libraryfolders.vdf'
        if not listing.exists():
            continue
        libraries.append(root)
        libraries += [Path(m) for m in re.findall(r'"path"\s+"([^"]+)"', listing.read_text(errors='replace'))]
    for library in libraries:
        game = library / 'steamapps/common' / GAME_FOLDER
        if (game / 'SystemShock2Remastered.exe').exists():
            return game.resolve()
    return None


def download(url, dest, what):
    dest.parent.mkdir(parents=True, exist_ok=True)
    partial = dest.with_name(dest.name + '.part')
    request = urllib.request.Request(url, headers={'User-Agent': 'ss2rtx-install'})
    with urllib.request.urlopen(request, timeout=60) as response, open(partial, 'wb') as out:
        total = int(response.headers.get('Content-Length') or 0)
        done = 0
        while True:
            block = response.read(1 << 20)
            if not block:
                break
            out.write(block)
            done += len(block)
            if total and sys.stdout.isatty():
                print(f'\r  {what}: {done >> 20} of {total >> 20} MiB', end='', flush=True)
    print(('\r' if total and sys.stdout.isatty() else '') + f'  {what}: {done >> 20} MiB fetched')
    partial.replace(dest)


def ensure_runtime():
    """NVIDIA's runtime, from NVIDIA: it is not ours to hand out."""
    if (RUNTIME / 'd3d9.dll').exists():
        return
    archive = ROOT / 'runtime/downloads' / RUNTIME_URL.rsplit('/', 1)[1]
    if not archive.exists():
        print("fetching the RTX Remix runtime 1.6.1 from NVIDIA's release page")
        download(RUNTIME_URL, archive, 'runtime')
    if sha256(archive) != RUNTIME_SHA256:
        sys.exit(f'{archive} is not the file expected (SHA-256 differs); delete it and run this again')
    print('unpacking the runtime')
    RUNTIME.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(RUNTIME)
    if not (RUNTIME / 'd3d9.dll').exists():
        sys.exit('the runtime archive did not hold what was expected')


def ensure_adapter():
    if ADAPTER.exists():
        return
    print('no adapter built here: fetching the released dxgi.dll')
    try:
        download(ADAPTER_URL, ADAPTER, 'adapter')
    except Exception as error:
        sys.exit(f"could not fetch it ({error}).\nEither download dxgi.dll from the project's releases into {ADAPTER.parent},\n"
                 'or build it: python3 build.py adapter')


# Remix settings; the first two are required under Proton (README.md).
RTX_CONF = """# Written by ss2rtx install.py
rtx.dlssPreset = 2
rtx.dlfg.enable = False
# Remix's default GUI input method blocks the game's keyboard and mouse input when both
# share a process. Keep this off.
rtx.useNewGuiInputMethod = False
# Without this Remix's texture streaming leaves every texture at a small, blurred mip.
rtx.texturemanager.neverDowngradeTextures = True
# Remix adds these camera effects by default. The game has none of them and they read as
# blur; set them to True if you want them.
rtx.postfx.enableMotionBlur = False
rtx.postfx.enableChromaticAberration = False
rtx.postfx.enableVignette = False
# The game applies its own bloom to the finished frame; Remix's on top of it doubles the
# glow round every lamp.
rtx.bloom.enable = False
# Brightness as the engine's: a fixed exposure and no tone curve (1.65 stops is log2(pi),
# which undoes the adapter's light scale). For Remix's own look instead, with auto-exposure
# and its local tone mapper, remove these four lines; dark rooms then come out brighter.
rtx.tonemappingMode = 0
rtx.autoExposure.enabled = False
rtx.tonemap.tonemappingEnabled = False
rtx.tonemap.exposureBias = 1.65
rtx.zUp = True
rtx.sceneScale = 0.0328
"""

INI = """; ss2rtx adapter settings. Environment variables SS2RTX_<NAME> override these.
[ss2rtx]
; Folder with the Remix runtime (d3d9.dll), relative to the game folder.
RemixDir=ss2rtx\\remix
; F11 in game opens a settings panel that changes most of these and stores them here.
; 1: put the path-traced image into the game's frame. Scroll Lock toggles it in game;
; Insert marks a glitch (saves that frame for analysis into this folder).
Composite=1
; Engine light colour to Remix radiance. Higher is brighter.
LightScale=40
; Radius of the sphere lights that stand in for the engine's point lights, in game units.
LightRadius=0.2
; Brightness of full-bright object parts (lamp faces). 0.32 already shows as full white
; with the exposure in rtx.conf; more only adds light to the room.
Emissive=0.4
; Brightness of full-bright level surfaces and glow layers (screens, lit signs). They also
; light the room around them, so a high value brightens dark rooms that have screens.
ScreenEmissive=0.4
; How much of a lamp face's or screen's brightness it also casts as light (0 to 1). The
; engine's cast none; 1 lets them light their rooms fully.
EmissiveLight=0.15
; The engine never leaves a surface darker than its ambient level; path traced, a corner no
; lamp reaches would be black. 1: surfaces give off that floor themselves. 0: off.
AmbientFloor=1
; DLSS mode. auto: full resolution up to 1200 lines of output, Remix's own choice above.
; conf: whatever rtx.conf says. 0-5: force (0 ultra performance, 3 quality, 5 full resolution).
Dlss=auto
"""

MATERIALS = """# ss2rtx material overrides. One line per texture, keyed by the hash listed in
# ss2rtx_cache\\textures_seen.txt (also the name of the exported .dds). Edit while the game
# runs: the file is re-read when it changes.
# Keys: roughness, metallic, emissive_intensity, displace, albedo, normal, roughness_tex,
#       metallic_tex, height, emissive (a .dds path relative to this file, or "self").
default roughness=0.75 metallic=0
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def game_running():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if b'SystemShock2Remastered.exe' in (proc / 'cmdline').read_bytes():
                return True
        except OSError:
            pass
    return False


def put(target, rel, manifest, *, source=None, text=None, keep_existing=False):
    """Place one file, backing up whatever was there."""
    dest = target / rel
    if dest.exists() and keep_existing:
        # 'ours': a file only we ever create (the settings file, which the in-game panel also
        # writes). It is left as it is, and still removed on uninstall.
        manifest['added' if keep_existing == 'ours' else 'kept'].append(rel)
        return
    if dest.exists():
        backup = target / 'ss2rtx/backup' / rel
        backup.parent.mkdir(parents=True, exist_ok=True)
        if not backup.exists():           # never overwrite the first, original backup
            shutil.copy2(dest, backup)
        manifest['replaced'].append({'file': rel, 'backup': str(Path('ss2rtx/backup') / rel), 'original_sha256': sha256(backup)})
    else:
        manifest['added'].append(rel)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if text is not None:
        dest.write_text(text)
    else:
        shutil.copy2(source, dest)


def install(target):
    if not (target / 'SystemShock2Remastered.exe').exists():
        sys.exit(f'{target} is not a System Shock 2 Remastered folder')
    ensure_adapter()
    ensure_runtime()
    manifest_path = target / 'ss2rtx/install-manifest.json'
    settings = target / 'ss2rtx/ss2rtx.ini'
    stored = settings.read_text() if settings.exists() else None   # what the settings panel has stored survives a re-install
    if manifest_path.exists():
        uninstall(target, quiet=True)   # re-install cleanly; original backups are restored first
    if stored is not None:
        settings.parent.mkdir(exist_ok=True)
        settings.write_text(stored)
    manifest = {'utc': datetime.now(timezone.utc).isoformat(), 'target': str(target), 'added': [], 'replaced': [], 'kept': [],
                'added_dirs': [], 'adapter_sha256': sha256(ADAPTER), 'remix_runtime': RUNTIME.name}
    (target / 'ss2rtx').mkdir(exist_ok=True)
    put(target, 'dxgi.dll', manifest, source=ADAPTER)
    put(target, 'rtx.conf', manifest, text=RTX_CONF)
    put(target, 'ss2rtx/ss2rtx.ini', manifest, text=INI, keep_existing='ours')   # keeps what the settings panel stored
    # materials.txt is the user's own work: created if missing, never removed except by --purge.
    if not (target / 'ss2rtx/materials.txt').exists():
        (target / 'ss2rtx/materials.txt').write_text(MATERIALS)
    runtime_dest = target / 'ss2rtx/remix'
    if runtime_dest.exists():
        shutil.rmtree(runtime_dest)
    # Reflink copy: no extra disk space on btrfs, an ordinary copy elsewhere.
    subprocess.run(['cp', '-a', '--reflink=auto', str(RUNTIME), str(runtime_dest)], check=True)
    manifest['added_dirs'].append('ss2rtx/remix')
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'installed into {target}')
    print(f"  added {len(manifest['added'])} files, replaced {len(manifest['replaced'])} (backed up), kept {len(manifest['kept'])}")
    for r in manifest['replaced']:
        print(f"  replaced {r['file']} (backup: {r['backup']})")
    print("\nSet the game's Steam launch options (Properties > General) to:\n\n  " + LAUNCH_OPTIONS +
          '\n\nand its renderer to Direct3D 11. In game: F11 settings, Scroll Lock path tracing on/off.')


def uninstall(target, purge=False, quiet=False):
    manifest_path = target / 'ss2rtx/install-manifest.json'
    if not manifest_path.exists():
        if not quiet:
            print('nothing installed here')
        return
    manifest = json.loads(manifest_path.read_text())
    for rel in manifest['added']:
        (target / rel).unlink(missing_ok=True)
    for d in manifest['added_dirs']:
        shutil.rmtree(target / d, ignore_errors=True)
    for r in manifest['replaced']:
        backup = target / r['backup']
        if backup.exists():
            shutil.copy2(backup, target / r['file'])
            if sha256(target / r['file']) != r['original_sha256']:
                sys.exit(f"restore of {r['file']} does not match its recorded hash; stopping")
        elif not quiet:
            print(f"warning: backup for {r['file']} is missing; left as is")
    shutil.rmtree(target / 'ss2rtx/backup', ignore_errors=True)
    manifest_path.unlink()
    # Files Remix and the adapter create while running.
    leftovers = ['ss2rtx_cache', 'rtx-remix', '.sentry-native', 'ss2rtx/ss2rtx.log', 'remix-dxvk.log', 'metrics.txt']
    if purge:
        leftovers.append('ss2rtx')
    for rel in leftovers if purge else leftovers[3:]:
        p = target / rel
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)
        else:
            p.unlink(missing_ok=True)
    if not quiet:
        print(f'uninstalled from {target}' + (' (caches and materials.txt removed too)' if purge
                                              else '; ss2rtx/materials.txt and the texture cache were left in place'))


def status(target):
    manifest_path = target / 'ss2rtx/install-manifest.json'
    if not manifest_path.exists():
        print('not installed')
        return
    m = json.loads(manifest_path.read_text())
    current = sha256(target / 'dxgi.dll') if (target / 'dxgi.dll').exists() else None
    print(f"installed {m['utc']}, runtime {m['remix_runtime']}")
    print('adapter dxgi.dll:', 'matches the installed build' if current == m['adapter_sha256'] else 'DIFFERS from the installed build')
    for r in m['replaced']:
        print(f"replaced {r['file']}; original kept at {r['backup']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=['install', 'uninstall', 'status', 'launch-options'])
    p.add_argument('--target', type=Path, help='game folder')
    p.add_argument('--steam', action='store_true', help="use the Steam installation as the target (found through Steam's library list)")
    p.add_argument('--purge', action='store_true', help='on uninstall, also remove caches and your materials.txt')
    a = p.parse_args()
    if a.action == 'launch-options':
        print(LAUNCH_OPTIONS)
        return
    if a.steam == bool(a.target):
        p.error('give exactly one of --target or --steam')
    if a.steam:
        target = find_game()
        if not target:
            sys.exit(f'{GAME_FOLDER} was not found in any Steam library; give its folder with --target')
    else:
        target = a.target.resolve()
    if a.action != 'status' and game_running():
        sys.exit('System Shock 2 is running; close it first')
    if a.action == 'install':
        install(target)
    elif a.action == 'uninstall':
        uninstall(target, purge=a.purge)
    else:
        status(target)


if __name__ == '__main__':
    main()
