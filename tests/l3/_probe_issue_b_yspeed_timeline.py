"""Issue B phase 3: full $7D timeline post-GM07 on both sides.
Sample every frame for 30 frames, print the divergence pattern.
Then arm a wider trace and re-step the divergence frame to capture
the (hopefully complete) writer chain."""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _read_byte(sock, f, addr, side):
    if side == 'rec':
        r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    else:
        r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _sample(sock, f):
    return {
        'YPos': (_read_byte(sock, f, 0xD4, 'rec') << 8) | _read_byte(sock, f, 0xD3, 'rec'),
        'YPosE': (_read_byte(sock, f, 0xD4, 'emu') << 8) | _read_byte(sock, f, 0xD3, 'emu'),
        'YNext': (_read_byte(sock, f, 0x97, 'rec') << 8) | _read_byte(sock, f, 0x96, 'rec'),
        'YNextE': (_read_byte(sock, f, 0x97, 'emu') << 8) | _read_byte(sock, f, 0x96, 'emu'),
        'YSpd': _read_byte(sock, f, 0x7D, 'rec'),
        'YSpdE': _read_byte(sock, f, 0x7D, 'emu'),
        'XPos': (_read_byte(sock, f, 0xD2, 'rec') << 8) | _read_byte(sock, f, 0xD1, 'rec'),
        'XPosE': (_read_byte(sock, f, 0xD2, 'emu') << 8) | _read_byte(sock, f, 0xD1, 'emu'),
        'Pose': _read_byte(sock, f, 0x13E0, 'rec'),
        'PoseE': _read_byte(sock, f, 0x13E0, 'emu'),
        'Input1': _read_byte(sock, f, 0x16, 'rec'),
        'Input1E': _read_byte(sock, f, 0x16, 'emu'),
        'Input2': _read_byte(sock, f, 0x17, 'rec'),
        'Input2E': _read_byte(sock, f, 0x17, 'emu'),
    }


def main():
    _kill(); time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'], cwd=str(REPO),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket()
        for _ in range(60):
            try:
                sock.connect(('127.0.0.1', PORT)); break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.2)
        f = sock.makefile('r'); f.readline()
        cmd(sock, f, 'pause')

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            if _read_byte(sock, f, 0x100, 'emu') == 0x07: break

        print(' fr | rX     | eX     | rY     | eY     | rN     | eN     | rS  | eS  | rPose | rIn1/2 | eIn1/2')
        for fi in range(0, 30):
            s = _sample(sock, f)
            mark = ''
            if s['YPos'] != s['YPosE']: mark += 'Y'
            if s['YSpd'] != s['YSpdE']: mark += 'S'
            if s['Input1'] != s['Input1E'] or s['Input2'] != s['Input2E']: mark += 'I'
            print(f' +{fi:2d} | r${s["XPos"]:04x} | e${s["XPosE"]:04x} | '
                  f'${s["YPos"]:04x} | ${s["YPosE"]:04x} | '
                  f'${s["YNext"]:04x} | ${s["YNextE"]:04x} | '
                  f'${s["YSpd"]:02x} | ${s["YSpdE"]:02x} | '
                  f'${s["Pose"]:02x}/${s["PoseE"]:02x} | '
                  f'${s["Input1"]:02x}/${s["Input2"]:02x} | '
                  f'${s["Input1E"]:02x}/${s["Input2E"]:02x}  {mark}')
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
