"""Count invocations of UploadLevelLayer1And2Tilemaps, BufferScrollingTiles_*
during step-to-frame 96 in recomp. Oracle doesn't report recomp names so we
only measure recomp.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 96
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); break
        except OSError:
            time.sleep(0.2)
    time.sleep(0.3)
    r = DebugClient(RECOMP_PORT)
    try:
        r.cmd('pause')
        r.cmd('profile_on')
        base = r.cmd('frame').get('frame', 0)
        r.cmd(f'step {target}')
        deadline = time.time() + 60
        while time.time() < deadline:
            if r.cmd('frame').get('frame', 0) >= base + target: break
            time.sleep(0.1)
        r.cmd('profile_off')
        prof = r.cmd('profile')
        want = ('UploadLevelLayer1And2Tilemaps',
                'InitializeLevelLayer1And2Tilemaps',
                'BufferScrollingTiles_Layer1_Init',
                'BufferScrollingTiles_Layer2_Init',
                'BufferScrollingTiles_Layer1',
                'BufferScrollingTiles_Layer2',
                'BufferScrollingTiles_Layer1_NoScroll',
                'BufferScrollingTiles_Layer1_VerticalLevel',
                'BufferScrollingTiles_Layer2_NoScroll',
                'BufferScrollingTiles_Layer2_VerticalLevel',
                'BufferScrollingTiles_Layer2_Background')
        print(f'frame={prof.get("frame_num")} funcs_tracked={prof.get("funcs")}')
        found = {e['name']: e['calls'] for e in prof.get('top', [])}
        print('\nTop 20 by calls (profile exposes top 20):')
        for e in prof.get('top', []):
            mark = '  *' if e['name'] in want else '   '
            print(f'{mark} {e["name"]:45s} calls={e["calls"]}')
        print('\nTargets not in top-20 (may be zero):')
        for n in want:
            if n not in found:
                print(f'     {n}: not in top-20')
    finally:
        r.close(); _kill()


if __name__ == '__main__':
    main()
