"""Measure how many iterations SMW's SPC handshake busy-wait
runs on each side during boot.

The SPC700UploadLoop at bank_00:$8082 polls $2140 expecting the
SPC IPL to echo $BBAA. Each iteration: BIT/CMP $2140 + branch.
On real hardware, the IPL takes ~30 cycles to be ready, so the
busy-wait runs maybe 1-3 iterations before exiting.

If recomp and snes9x match, SPC handshake isn't the cycle-
accuracy gap. If recomp does 1 iter and snes9x does 100s, it IS.
"""
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

        # Arm recomp's block trace so we can count SPC busy-wait
        # iterations by counting block-enters at $8082 (the wait label).
        cmd(sock, f, 'trace_blocks')
        # Arm snes9x's insn trace at the same PC range.
        cmd(sock, f, 'emu_insn_trace_on 0x008082 0x008087')

        # Step until recomp reaches GM=07 (lockstep).
        rs = 0
        for _ in range(3000):
            cmd(sock, f, 'step 1'); rs += 1
            if int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 0x07:
                break
        ef = cmd(sock, f, 'emu_frame').get('frame', '?')
        print(f'[at rec GM=07] rec_steps={rs} emu_frame={ef}')

        # Recomp side: count block enters at $8082.
        bt = cmd(sock, f, 'get_block_trace')
        rec_iters = sum(1 for e in bt.get('log', [])
                        if e.get('pc') == '0x008082')
        print(f'  recomp $8082 block enters: {rec_iters}')

        # Emu side: count insn trace at $8082-87.
        et = cmd(sock, f, 'emu_get_insn_trace')
        et_log = et.get('log', [])
        emu_iters = sum(1 for e in et_log
                        if e.get('pc', '').startswith('0x008082'))
        print(f'  emu $8082 insn enters: {emu_iters} '
              f'(total trace entries: {len(et_log)})')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
