"""Attract-demo behavioral regression test.

Boots the Oracle build (recomp + embedded snes9x), steps recomp
through the attract demo, and asserts a hand-curated set of
INVARIANTS — visible behaviors we have explicitly confirmed
correct. Each invariant is a cross-checkable WRAM-state
proposition. Other state is intentionally NOT asserted, so
fixing currently-broken behavior doesn't trip the test.

Why this shape (not snapshot-diff): a snapshot-diff approach
locks in EVERYTHING about the current state — including
bugs we haven't fixed yet (Yoshi-floats-up, koopa-invisible-2nd-
cycle, etc.). That makes the regression test fight you the
moment you fix one of those bugs. Field-level invariants
encode just the behaviors we know are right, leave the rest
unasserted, and never block legitimate forward progress.

Adding new invariants:
  1. Visually confirm the new behavior is correct.
  2. Identify the WRAM state that uniquely captures it.
  3. Add an `Inv(...)` entry below with a clear docstring.

Skipped when build/bin-x64-Oracle/smw.exe isn't present.
"""
from __future__ import annotations
import dataclasses
import json
import pathlib
import socket
import subprocess
import sys
import time
from typing import Callable, List, Optional

REPO = pathlib.Path(__file__).absolute().parents[2]
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def _launch():
    if not ORACLE_EXE.exists():
        return None
    _kill()
    time.sleep(0.5)
    p = subprocess.Popen(
        [str(ORACLE_EXE), '--paused'],
        cwd=str(REPO),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(
                ('127.0.0.1', PORT), timeout=0.3)
            s.close()
            time.sleep(0.3)
            return p
        except OSError:
            time.sleep(0.2)
    p.kill()
    raise RuntimeError('attract regression: oracle TCP timeout')


class _Client:
    def __init__(self, port):
        self.sock = socket.create_connection(
            ('127.0.0.1', port), timeout=600)
        self.f = self.sock.makefile('rwb')
        self.f.readline()
    def cmd(self, line):
        self.sock.sendall((line + '\n').encode())
        return json.loads(self.f.readline())
    def close(self):
        try: self.sock.close()
        except OSError: pass


def _step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target:
        return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        cur = c.cmd('frame').get('frame', cur)
        if cur >= target:
            break
        time.sleep(0.05)
    return cur


def _read_bytes(c, addr, n):
    return bytes.fromhex(c.cmd(f'read_ram {addr:x} {n}').get('hex', ''))


# ----------------------------------------------------------------------
# Invariant DSL
# ----------------------------------------------------------------------
#
# Each invariant is an Inv(name, frame, predicate, why). At the given
# frame, we check predicate(client) returning (ok: bool, detail: str).
# If ok is False, the failure message includes name + why + detail
# so a regression points straight at what visible behavior broke.

@dataclasses.dataclass
class Inv:
    name: str
    frame: int
    predicate: Callable[['_Client'], tuple]  # returns (ok, detail)
    why: str  # one-line description of the visible behavior


# ----- predicates -----------------------------------------------------
#
# Predicates are plain functions over WRAM. Keep each one focused on
# ONE proposition so a single-bit regression names a single invariant.

def _game_mode_attract_demo_active(c):
    """Attract demo at the title screen runs game mode $05 (the
    title-screen / demo state). Without this, the recomp didn't even
    reach the demo, so every later invariant is moot. The demo
    eventually transitions to other modes when restarting; for
    the early-frame check we just want "we're past boot."
    """
    gm = _read_bytes(c, 0x0100, 1)[0]
    # Mode $05 = title-screen / attract demo. Modes $13-$15 are
    # active gameplay (the demo plays a level). All non-zero non-
    # boot modes count as "got past boot."
    ok = gm not in (0x00, 0x01, 0x02, 0x03, 0x04)
    return ok, f'game_mode=$0100=${gm:02X}'


def _at_least_one_sprite_alive(c):
    """At frame 100+ the attract level should have spawned at least
    one sprite. If $14C8..$14D3 are all zeros, the sprite-spawn path
    is broken (koopa-invisible-on-2nd-cycle is detected in a
    separate, more specific invariant)."""
    statuses = _read_bytes(c, 0x14C8, 12)
    alive = sum(1 for s in statuses if s != 0)
    return alive >= 1, f'sprite_status=$14C8=[{statuses.hex()}] alive={alive}'


def _sprite_with_status_present(c):
    """Slot table progresses — at frame 700 (deep into the demo),
    multiple sprites should have non-zero status. Catches the
    "all sprites missing on 2nd attract cycle" class even though we
    don't explicitly mark which one is the koopa."""
    statuses = _read_bytes(c, 0x14C8, 12)
    alive = sum(1 for s in statuses if s != 0)
    return alive >= 2, f'sprite_status=$14C8=[{statuses.hex()}] alive={alive}'


def _yoshi_egg_spawns_at_some_frame(c):
    """Yoshi egg = sprite type $2D. Should appear in some slot
    by frame 700. Without this, the ?-block→egg spawn path is
    broken (the "egg vanishes immediately" class)."""
    types = _read_bytes(c, 0x009E, 12)
    has_egg = 0x2D in types
    return has_egg, f'sprite_type=$009E=[{types.hex()}] has_egg={has_egg}'


def _yoshi_spawns_at_some_frame(c):
    """Yoshi himself = sprite type $35. Should appear by frame 900.
    Without this, the egg-hatch path is broken."""
    types = _read_bytes(c, 0x009E, 12)
    has_yoshi = 0x35 in types
    return has_yoshi, f'sprite_type=$009E=[{types.hex()}] has_yoshi={has_yoshi}'


def _yoshi_does_not_float_up(c):
    """Issue C closure (2026-04-26): after Yoshi spawns from a ?-block
    he must obey gravity, not rise indefinitely. Pre-fix, phantom auto-
    promotion at $01:ECEC fragmented Spr035_Yoshi's body and re-applied
    the on-ground init `Y velocity = $F0` (= -16, upward) every frame.
    Post-fix the natural-fall-through predicate suppresses the phantom
    tail-call.

    Check: at frame 950 (≈50f after Yoshi spawn at frame 900), Yoshi's
    Y position must be at or below where he spawned. Y addresses are
    $00D8 (lo) / $14D4 (hi) per slot, indexed by the slot holding type
    $35.
    """
    types = _read_bytes(c, 0x009E, 12)
    slot = next((i for i, t in enumerate(types) if t == 0x35), None)
    if slot is None:
        return False, f'no Yoshi slot at frame 950; types={types.hex()}'
    y_lo = _read_bytes(c, 0x00D8 + slot, 1)[0]
    y_hi = _read_bytes(c, 0x14D4 + slot, 1)[0]
    y = (y_hi << 8) | y_lo
    # Yoshi-floats-up bug pushes Y far above the screen (toward 0 or
    # underflow toward $FFFF). On-ground Yoshi sits roughly at Y=$00B0
    # in the ?-block scene. Pre-fix this hit Y < $0080 within ~40
    # frames; post-fix Y stays ≥ $00A0. Pin a permissive lower bound
    # so unrelated Y-physics tweaks don't trip the test.
    ok = y >= 0x0080
    return ok, (f'yoshi slot={slot} y=${y:04X} (must stay >= $0080 '
                f'i.e. not floated above screen)')


def _mario_x_advances(start_frame, end_frame):
    """Returns a predicate that captures Mario's X at construction
    and asserts at predicate-call time that X > captured. Used to
    pin "Mario doesn't get stuck" (koopa-stomp-but-frozen, etc.).
    Currently not used by the invariants list — kept as an example
    of how a multi-frame invariant would be expressed if needed."""
    raise NotImplementedError('multi-frame invariants TBD')


# ----- the invariants list -----------------------------------------
#
# Order: by frame ascending, so failures read top-to-bottom in
# play order.

INVARIANTS: List[Inv] = [
    Inv(
        name='attract_demo_past_boot',
        frame=100,
        predicate=_game_mode_attract_demo_active,
        why='Demo gets past the boot/init game modes. Without this '
            'the recomp wedged before the title-screen demo started.',
    ),
    Inv(
        name='at_least_one_sprite_alive_early',
        frame=300,
        predicate=_at_least_one_sprite_alive,
        why='Sprite spawn pipeline produces at least one live slot '
            'by frame 300. Catches the "all sprites missing" class.',
    ),
    Inv(
        name='yoshi_spawns_in_demo',
        frame=900,
        predicate=_yoshi_spawns_at_some_frame,
        why='Yoshi ($35) appears in some sprite slot by frame 900. '
            'Closes the "Yoshi-egg hatch path silently breaks" '
            'regression class. (Egg-state intermediate is too fast '
            'to catch at fixed frames; final-Yoshi presence is the '
            'observable proxy.)',
    ),
    Inv(
        name='yoshi_does_not_float_up',
        frame=950,
        predicate=_yoshi_does_not_float_up,
        why='Issue C closure: after spawn, Yoshi obeys gravity instead '
            'of rising forever. User-confirmed visually 2026-04-26 '
            'after the natural-fall-through predicate suppressed the '
            'phantom auto_01_ECEC tail-call.',
    ),
    # NOT YET INVARIANT (open bugs — DO NOT lock in):
    #   - koopa-visible-on-2nd-attract-cycle (Issue A): the visible
    #     bug is rendering, not state — needs OAM/CGRAM check.
    #   - mario-Y-stable-near-?-block (Issue B): visible Y bug, but
    #     Mario's Y oscillates normally during demo — need a
    #     specific frame range where he should be on flat ground.
    #   - bg-slope-no-spurious-dirt-tiles (Issue D): tile state, not
    #     sprite state — needs VRAM/tilemap check.
    # Add each as an Inv(...) entry once visually confirmed fixed.
]


def test_attract_demo_invariants_hold():
    if not ORACLE_EXE.exists():
        return  # build absent — skip
    proc = _launch()
    if proc is None:
        return
    failures = []
    try:
        c = _Client(PORT)
        try:
            # Sort invariants by frame so we step monotonically.
            for inv in sorted(INVARIANTS, key=lambda i: i.frame):
                _step_to(c, inv.frame)
                ok, detail = inv.predicate(c)
                if not ok:
                    failures.append(
                        f'  [{inv.name}] frame {inv.frame} FAILED.\n'
                        f'      why: {inv.why}\n'
                        f'      detail: {detail}'
                    )
        finally:
            c.close()
    finally:
        proc.kill()
        _kill()
    if failures:
        msg = ('Attract-demo behavioral invariant failures '
               f'({len(failures)}/{len(INVARIANTS)}):\n'
               + '\n'.join(failures)
               + '\n\nIf the visible behavior these invariants encode '
               'has been intentionally retired, edit INVARIANTS in '
               f'{pathlib.Path(__file__).name} to remove the '
               'corresponding Inv(...) entry. Do NOT loosen the '
               'predicate to "always pass" — that defeats the '
               'whole point of a regression test.')
        assert False, msg


if __name__ == '__main__':
    try:
        test_attract_demo_invariants_hold()
        print('PASS')
    except AssertionError as e:
        print('FAIL:')
        print(str(e)[:8000])
