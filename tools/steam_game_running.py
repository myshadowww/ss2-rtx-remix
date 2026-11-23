#!/usr/bin/env python3
"""Exit status 0 if the Steam copy of the game is running (as opposed to the private test
copy), 1 if not. Looks at each process's own program name, so a shell whose command line
merely mentions the game does not count."""
import sys
from pathlib import Path


def running():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            argv0 = (proc / 'cmdline').read_bytes().split(b'\0')[0].decode('utf-8', 'replace')
        except OSError:
            continue
        if argv0.endswith('SystemShock2Remastered.exe') and 'steamapps' in argv0.replace('\\', '/'):
            return True
    return False


if __name__ == '__main__':
    sys.exit(0 if running() else 1)
