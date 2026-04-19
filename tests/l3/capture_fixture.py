"""Capture a reference fixture snapshot by advancing the oracle N frames
then save_state'ing into the fixtures directory.

Usage:  python capture_fixture.py <name> <frame>
Result: snesrecomp/tests/l3/fixtures/<name>.snap
"""
import json
import pathlib
import socket
import subprocess
import sys
import time


# Note: do NOT call .resolve() — `snesrecomp` is a junction inside the SMW
# repo; resolving follows it out to F:\Projects\snesrecomp and breaks the
# parent chain below.
REPO = pathlib.Path(__file__).parent.parent.parent.parent
FIXTURES = REPO / 'snesrecomp' / 'tests' / 'l3' / 'fixtures'
ORACLE_EXE = REPO.parent / 'SuperMarioWorldRecomp-oracle' / 'build' / 'bin-x64-Release' / 'smw.exe'
PORT = 4378


def cmd(s, f, line):
    s.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step_to(s, f, target):
    cur = int(cmd(s, f, 'history')['history'].get('newest', -1))
    if cur >= target:
        return
    cmd(s, f, f'step {target - cur}')
    deadline = time.time() + 180
    while time.time() < deadline:
        cur = int(cmd(s, f, 'history')['history'].get('newest', -1))
        if cur >= target:
            return
        time.sleep(0.3)
    raise RuntimeError(f'stuck at frame {cur}, wanted {target}')


def main():
    if len(sys.argv) != 3:
        print('usage: capture_fixture.py <name> <frame>', file=sys.stderr)
        sys.exit(2)
    name = sys.argv[1]
    frame = int(sys.argv[2])
    FIXTURES.mkdir(exist_ok=True, parents=True)
    out_path = (FIXTURES / f'{name}.snap').as_posix()

    subprocess.run('taskkill /F /IM smw.exe',
                   capture_output=True, check=False, shell=True)
    time.sleep(0.5)
    oracle_exe_w = str(ORACLE_EXE).replace('/', '\\')
    oracle_cwd_w = str(ORACLE_EXE.parent.parent.parent).replace('/', '\\')
    if not ORACLE_EXE.exists():
        raise FileNotFoundError(f'oracle binary not found: {ORACLE_EXE}')
    subprocess.Popen(
        [oracle_exe_w, '--paused'],
        cwd=oracle_cwd_w,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(3)

    s = socket.create_connection(('127.0.0.1', PORT), timeout=30)
    f = s.makefile('rwb')
    f.readline()
    try:
        step_to(s, f, frame)
        r = cmd(s, f, f'save_state {out_path}')
        if not r.get('ok'):
            raise RuntimeError(f'save_state failed: {r}')
        print(f'captured {out_path} ({r["bytes"]} bytes) at frame {frame}')
    finally:
        s.close()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       capture_output=True, check=False)


if __name__ == '__main__':
    main()
