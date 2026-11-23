#!/usr/bin/env python3
"""Create the private game copy and profile that all ss2rtx development runs use.

The Steam install is only read. The copy is a btrfs reflink, so it costs no disk space
until files diverge. Files added by the SS2GraphicsHook mod are left out, so the copy is
the stock game plus whatever launch.py installs.

  python3 setup_game.py            # create or refresh game/ and the profile
  python3 setup_game.py --status
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import install

ROOT = Path(__file__).resolve().parent
PROFILE_SOURCE = ROOT.parent / 'dx11/prefix/pfx/drive_c/users/steamuser/Saved Games/Nightdive Studios/System Shock 2 Remastered'
PROFILE_REL = Path('pfx/drive_c/users/steamuser/Saved Games/Nightdive Studios/System Shock 2 Remastered')
GAME = ROOT / 'game'
MANIFEST = ROOT / 'game.json'

# Not part of the stock game: the SS2GraphicsHook mod, its helper and the runtimes it needs.
MOD_FILES = ['dxgi.dll', 'd3dcompiler_47.dll', 'ss2_ms_d3dcompiler_47.dll', 'nvngx_dlss.dll', 'nvngx_dlssnr.dll',
             'SS2DLSS.ini', 'SS2NRControls.ini', 'SS2NRControls.log', 'SS2GraphicsHook.log', 'README.md',
             'DLSSNR_REFERENCE_LICENSE.txt', 'NVIDIA_RTX_SDK_LICENSE.txt', 'THIRD_PARTY_NOTICES.txt', 'SS2ShaderCache',
             'SS2ShaderDump',
             # ...and anything an ss2rtx install in the Steam folder has put there: the test copy
             # gets the adapter from launch.py and must not inherit the player's settings.
             'ss2rtx', 'ss2rtx_cache', 'rtx.conf', 'rtx-remix', 'remix-shim.log', 'metrics.txt', '.sentry-native']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def set_cfg(text, key, value):
    text, n = re.subn(rf'^(seta {re.escape(key)} )"[^"]*"', lambda m: f'{m[1]}"{value}"', text, flags=re.M)
    if n != 1:
        sys.exit(f'profile setting {key} not found exactly once')
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--status', action='store_true')
    p.add_argument('--resolution', default='1280x720', help='window size for development runs')
    a = p.parse_args()
    if a.status:
        print(MANIFEST.read_text() if MANIFEST.exists() else 'not set up')
        return
    STEAM_GAME = install.find_game()
    if not STEAM_GAME:
        sys.exit(f'{install.GAME_FOLDER} was not found in any Steam library')

    if GAME.exists():
        shutil.rmtree(GAME)
    subprocess.run(['cp', '-a', '--reflink=auto', str(STEAM_GAME), str(GAME)], check=True)
    removed = []
    for name in MOD_FILES:
        target = GAME / name
        if target.is_dir():
            shutil.rmtree(target)
            removed.append(name + '/')
        elif target.exists():
            target.unlink()
            removed.append(name)

    profile = ROOT / 'prefix' / PROFILE_REL
    profile_note = 'kept existing profile'
    if not profile.exists():
        if not PROFILE_SOURCE.exists():
            sys.exit(f'no profile to copy at {PROFILE_SOURCE}')
        profile.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(PROFILE_SOURCE, profile)
        profile_note = f'copied from {PROFILE_SOURCE}'
    cfg = profile / 'kexengine.cfg'
    text = cfg.read_text()
    w, h = a.resolution.split('x')
    text = set_cfg(text, 'v_width', w)
    text = set_cfg(text, 'v_height', h)
    text = set_cfg(text, 'r_rhirenderfamily', 'd3d11')
    cfg.write_text(text)

    manifest = {
        'utc': datetime.now(timezone.utc).isoformat(),
        'source': str(STEAM_GAME),
        'game': str(GAME),
        'exe_sha256': sha256(GAME / 'SystemShock2Remastered.exe'),
        'mod_files_left_out': removed,
        'profile': str(profile),
        'profile_note': profile_note,
        'profile_settings': {'v_width': w, 'v_height': h, 'r_rhirenderfamily': 'd3d11'},
        'saves': sorted(x.name for x in profile.glob('*.sav')),
    }
    MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
