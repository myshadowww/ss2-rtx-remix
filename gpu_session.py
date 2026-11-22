#!/usr/bin/env python3
"""Free the GPUs for a block of Remix tests, then give them back.

  python3 gpu_session.py begin    # stop the service named in local.json, if it is running
  python3 gpu_session.py end      # start it again, only if `begin` stopped it
  python3 gpu_session.py status

For machines where something else (a model server, say) normally fills the GPU memory.
local.json, next to this script and not part of the repository, names it:

  { "pause_service": "some-unit.service" }      # a systemd --user unit

Without that file this script only reports. The service's own settings are never
changed. State is kept in run/gpu-session.json.
"""
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STATE = ROOT / 'run/gpu-session.json'
LOCAL = ROOT / 'local.json'
SERVICE = (json.loads(LOCAL.read_text()).get('pause_service') if LOCAL.exists() else None) or None


def active_state():
    if not SERVICE:
        return 'none configured'
    r = subprocess.run(['systemctl', '--user', 'show', '--property=ActiveState', '--value', SERVICE],
                       capture_output=True, text=True)
    return r.stdout.strip()


def vram():
    out = subprocess.check_output(['nvidia-smi', '--query-gpu=index,memory.used,memory.total',
                                   '--format=csv,noheader,nounits'], text=True)
    return [tuple(int(x) for x in line.split(',')) for line in out.strip().splitlines()]


def save(**kw):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    kw['utc'] = datetime.now(timezone.utc).isoformat()
    STATE.write_text(json.dumps(kw, indent=2) + '\n')


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'status'
    if not SERVICE:
        cmd = 'status'   # nothing to pause on this machine
    state = json.loads(STATE.read_text()) if STATE.exists() else {}
    if cmd == 'begin':
        if state.get('we_stopped_it') and active_state() != 'active':
            print('already paused by an earlier begin')
        elif active_state() in ('active', 'activating', 'reloading'):
            subprocess.run(['systemctl', '--user', 'stop', SERVICE], check=True)
            save(service=SERVICE, we_stopped_it=True)
            for _ in range(30):  # wait for the container to release its memory
                if all(used < 8000 for _, used, _ in vram()):
                    break
                time.sleep(1)
            print('paused', SERVICE)
        else:
            save(service=SERVICE, we_stopped_it=False)
            print(SERVICE, 'was not running; leaving it stopped')
    elif cmd == 'end':
        if state.get('we_stopped_it'):
            r = subprocess.run(['systemctl', '--user', 'start', SERVICE], capture_output=True, text=True)
            save(service=SERVICE, we_stopped_it=False, restart_returncode=r.returncode,
                 restart_output=(r.stdout + r.stderr).strip())
            print('restart requested' if r.returncode == 0 else f'RESTART FAILED: {r.stderr.strip()}')
        else:
            print('nothing to restore')
    print('service:', active_state(), '| VRAM MiB used/total:', ' '.join(f'gpu{i} {u}/{t}' for i, u, t in vram()))


if __name__ == '__main__':
    main()
