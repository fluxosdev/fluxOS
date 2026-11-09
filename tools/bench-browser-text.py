#!/usr/bin/env python3
"""Repeat the production font-width benchmark, keeping its font fixture intact."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
fixture = root / 'build/tests/font-test.fnt'
previous = fixture.read_bytes() if fixture.exists() else None
try:
    fixture.write_bytes((root / 'rootfs/usr/share/fonts/web.fnt').read_bytes())
    env = dict(os.environ, WAVE_PROFILE='0')
    for _ in range(3):
        subprocess.run([str(root / 'build/tests/browser-font'), 'bench'], env=env, check=True)
finally:
    if previous is None:
        fixture.unlink(missing_ok=True)
    else:
        fixture.write_bytes(previous)
