"""Run the real-ROM MMX state checks without touching a player's saves."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--rom', type=Path, required=True)
a = p.parse_args()
env = dict(os.environ, SNESRECOMP_REWIND_INTERVAL='1')
with tempfile.TemporaryDirectory(prefix='mmx-state-') as temp:
    for extra in ([], ['--resume']):
        result = subprocess.run([str(a.exe.resolve()), str(a.rom.resolve()), *extra],
                                cwd=temp, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=180,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        print(result.stdout, end='')
        result.check_returncode()
        if 'MMX STATE CHECKS PASSED' not in result.stdout:
            raise RuntimeError('Runtime check exited without its completion marker')
