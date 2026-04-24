"""Scan oracle slot statuses after mode 0x07 to find where the koopa
actually lives on the oracle side."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def e_byte(sock, f, addr):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


def e_bytes(sock, f, addr, n):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} {n}')
    return [int(b, 16) for b in r['hex'].split()] if r.get('ok') else []


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r'); f.readline()

        for i in range(2000):
            cmd(sock, f, 'emu_step 1')
            if e_byte(sock, f, 0x100) == TARGET_MODE:
                print(f'mode 0x07 at step {i+1}'); break

        # Sprite status array at 14C8-14D1 (12 slots).
        # Dwell 30 frames and print status + YPos at each frame.
        print('\n=== slot statuses at each of 30 dwell frames ===')
        print('     frame | status (12 slots) | nums (12)')
        for i in range(60):
            cmd(sock, f, 'emu_step 1')
            fr = cmd(sock, f, 'emu_frame').get('frame', '?')
            st = e_bytes(sock, f, 0x14C8, 12)
            nums = e_bytes(sock, f, 0x9E, 12)
            st_str = ' '.join(f'{b:02x}' for b in st)
            nums_str = ' '.join(f'{b:02x}' for b in nums)
            # Only print if any slot has a nonzero status.
            if any(b for b in st):
                print(f'  f{fr:>5} | {st_str} | {nums_str}')
        return 0
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
