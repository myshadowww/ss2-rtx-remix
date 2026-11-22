#!/usr/bin/env python3
"""Steam launch wrapper for playing System Shock 2 Remastered with ss2rtx installed.

Set this as the game's Steam launch options (Properties > General):

  /usr/bin/env -u LD_LIBRARY_PATH /usr/bin/python3 /path/to/ss2-rtx-remix/play.py -- %command%

It sets the environment the adapter and Remix need, then runs the game and waits for it.
If local.json (next to this script, not in the repository) names a systemd --user service
that normally fills the GPU memory, that service is stopped while the game runs and
started again afterwards, only if it was running; --keep-service before the -- leaves it
alone.

Requires `python3 install.py install --steam` first, and the game's renderer set to
Direct3D 11.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GAME = Path.home() / '.local/share/Steam/steamapps/common/System Shock 2 Remastered'
LOCAL = ROOT / 'local.json'   # optional, not in the repository: { "pause_service": "some-unit.service" }
SERVICE = (json.loads(LOCAL.read_text()).get('pause_service') if LOCAL.exists() else None) or None


def service_active():
    if not SERVICE:
        return False
    r = subprocess.run(['systemctl', '--user', 'show', '--property=ActiveState', '--value', SERVICE], capture_output=True, text=True)
    return r.stdout.strip() in ('active', 'activating', 'reloading')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--keep-service', action='store_true',
                   help='leave the service named in local.json running')
    p.add_argument('command', nargs=argparse.REMAINDER, help='the Steam %%command%% after --')
    a = p.parse_args()
    command = a.command[1:] if a.command and a.command[0] == '--' else a.command
    if not command:
        sys.exit('no game command given; this script is meant for the Steam launch options (see --help)')
    if not (GAME / 'ss2rtx/install-manifest.json').exists():
        sys.exit('ss2rtx is not installed in the Steam game folder: run  python3 install.py install --steam')

    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('DLSSNR_', 'SS2_NATIVE_', 'DLSS5VK_', 'SS2RTX_')) or key in ('VKLayer_DLSS5', 'VKLAYER_DLSS5', 'LD_PRELOAD'):
            env.pop(key, None)
    gpu = subprocess.check_output(['nvidia-smi', '-i', '0', '--query-gpu=uuid', '--format=csv,noheader'], text=True).strip()
    env.update(
        # The adapter (dxgi.dll) and the Remix runtime (d3d9.dll) must load as native DLLs.
        WINEDLLOVERRIDES='dxgi=n,b;d3d9=n,b',
        PROTON_FORCE_NVAPI='1', PROTON_ENABLE_WAYLAND='0',
        DLSSNR_DISABLE='1',
        DISABLE_VK_LAYER_VALVE_steam_overlay_1='1',
        DXVK_FILTER_DEVICE_UUID=gpu.removeprefix('GPU-').replace('-', '').lower(),   # GPU 0 is the one that can present
        # Remix's crash reporting is consent-gated and stays off; this also blocks its upload address.
        RTX_SENTRY_TEST_DSN='http://none@127.0.0.1:9/0',
    )

    paused = False
    if not a.keep_service and service_active():
        subprocess.run(['systemctl', '--user', 'stop', SERVICE], check=False)
        paused = True
    try:
        rc = subprocess.call(command, env=env)
    finally:
        if paused:
            subprocess.run(['systemctl', '--user', 'start', SERVICE], check=False)
    sys.exit(rc)


if __name__ == '__main__':
    main()
