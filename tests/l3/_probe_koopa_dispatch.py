"""Trace which collision-handler branches fire during the attract demo's
koopa-stomp moment. Hypothesis: recomp routes to HURT instead of STOMP.

Captures call trace filtered to CheckPlayerToNormalSprite* + DamagePlayer*
+ BoostMario* + SpawnContact* across attract-demo frames 200-280 (Mario
makes contact with the koopa around frame ~260 per investigation notes).
"""
import sys, pathlib, time, subprocess, socket, json
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError: time.sleep(0.2)
    raise RuntimeError('no connect')


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        # Run forward enough frames to cover the koopa-contact moment
        # (~260 per investigation notes; ~400 to also catch death music load).
        N = 400
        c.cmd(f'step {N}')
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                cur = c.cmd('frame').get('frame', 0)
                if cur >= N: break
            except Exception:
                break
            time.sleep(0.05)

        # Pull trace filtered to the collision/dispatch funcs of interest.
        results = {}
        for substr in ['CheckForContact', 'GetMarioClipping', 'GetSpriteClipping',
                       'BoostMario', 'SpawnContact',
                       'CheckPlayerToNormalSpriteColl_01A8',
                       'PlayerDying', 'TransitionToGameOver', 'GameMode09',
                       'PlayerFell', 'KillPlayer']:
            r = c.cmd(f'get_call_trace contains={substr}')
            log = r.get('log', [])
            results[substr] = log
            print(f'\n--- {substr}: {len(log)} hits ---')
            for e in log[:20]:
                print(f'  f{e["f"]:4} d{e["d"]:3} {e["func"]:50} parent={e["parent"]}')
            if len(log) > 20:
                print(f'  ... +{len(log)-20} more')
    finally:
        try: c.close()
        except Exception: pass
        _kill()


if __name__ == '__main__':
    main()
