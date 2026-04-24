"""Sanity-check emu_read_wram response format."""
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())

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
    f = sock.makefile('r'); banner = f.readline()
    print(f'banner: {banner!r}')

    for _ in range(500):
        cmd(sock, f, 'emu_step 1')

    print('\nemu_frame:', cmd(sock, f, 'emu_frame'))
    print('emu_read_wram 0x100 1:', cmd(sock, f, 'emu_read_wram 0x100 1'))
    print('emu_read_wram 0x14C8 12:', cmd(sock, f, 'emu_read_wram 0x14C8 12'))
    print('emu_read_wram 0x9E 12:', cmd(sock, f, 'emu_read_wram 0x9E 12'))
    print('frame:', cmd(sock, f, 'frame'))
finally:
    sock.close()
    proc.terminate()
    try: proc.wait(timeout=5)
    except subprocess.TimeoutExpired: proc.kill()
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
