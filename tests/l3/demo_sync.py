"""Demo-phase sync helper for attract-demo lockstep probes.

Problem: snes9x's `retro_init` / `retro_load_game` emulate the SNES
from cold-boot through some number of internal frames before the
first explicit `emu_step`. Recomp's `I_RESET` runs inside our first
`step`. Result: the two sides reach GM=$07 from different starting
points in the SNES timeline. Stepping both equally from "first GM=07
sighting" doesn't put them at the same demo phase — snes9x is
already partway through the title-screen input sequence.

Both sides are individually correct. The probe needs a sync point
defined in terms of ROM state, not step counts.

Solution: read each side's TitleInputIndex ($1DF4) +
VariousPromptTimer ($1DF5) — the demo's own phase tracker. Step
the lagging side until both pairs match. From there, lockstep
produces identical demo input sequences and any subsequent
divergence is a real codegen / runtime bug.

Reference: docs/VIRTUAL_HW_*.md (the architectural framing); SMWDisX
bank_00:3346-3375 (GM07TitleScreen + WriteControllerInput).
"""
from __future__ import annotations
import json
import time
from typing import Optional

# WRAM addresses derived from src/gen/smw_00_gen.c::GameMode07_TitleScreenDemo
# (decrement of $1df5, increment-by-2 of $1df4 each demo phase boundary).
TITLE_INPUT_INDEX = 0x1DF4
VARIOUS_PROMPT_TIMER = 0x1DF5


def _read_byte(sock, f, addr: int, side: str) -> int:
    """Read one byte from recomp ('rec') or oracle ('emu') WRAM."""
    cmd_str = (f'dump_ram 0x{addr:x} 1' if side == 'rec'
               else f'emu_read_wram 0x{addr:x} 1')
    sock.sendall((cmd_str + '\n').encode())
    r = json.loads(f.readline())
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _step(sock, f, side: str, n: int = 1) -> None:
    cmd_str = (f'step {n}' if side == 'rec' else f'emu_step {n}')
    sock.sendall((cmd_str + '\n').encode())
    _ = json.loads(f.readline())


def _read_demo_phase(sock, f, side: str) -> tuple[int, int]:
    """Returns (TitleInputIndex, VariousPromptTimer) on the named side."""
    return (_read_byte(sock, f, TITLE_INPUT_INDEX, side),
            _read_byte(sock, f, VARIOUS_PROMPT_TIMER, side))


def _phase_lt(a: tuple[int, int], b: tuple[int, int]) -> bool:
    """Demo-phase ordering: SMALLER TitleInputIndex = earlier in demo.
    Within the same TitleInputIndex, LARGER VariousPromptTimer = earlier
    (more time left in current phase before the next demo entry loads).
    """
    if a[0] != b[0]:
        return a[0] < b[0]
    return a[1] > b[1]


def step_both_to_gm07(sock, f, max_steps: int = 3000) -> tuple[int, int]:
    """Advance each side independently until $0100 == 0x07. Returns
    (rec_step_count, emu_step_count)."""
    rs = es = 0
    for _ in range(max_steps):
        _step(sock, f, 'rec')
        rs += 1
        if _read_byte(sock, f, 0x0100, 'rec') == 0x07:
            break
    for _ in range(max_steps):
        _step(sock, f, 'emu')
        es += 1
        if _read_byte(sock, f, 0x0100, 'emu') == 0x07:
            break
    return rs, es


def sync_demo_phase(sock, f, max_extra_steps: int = 3000,
                    verbose: bool = False) -> dict:
    """After both sides have reached GM=07, step the lagging side
    until (TitleInputIndex, VariousPromptTimer) match on both.

    Returns a dict with sync diagnostics:
      {'rec_phase': (idx, timer), 'emu_phase': (idx, timer),
       'extra_steps_rec': N, 'extra_steps_emu': M, 'synced': bool}

    If the sides can't be synced within max_extra_steps, returns
    synced=False and the closest reached state.
    """
    rec_phase = _read_demo_phase(sock, f, 'rec')
    emu_phase = _read_demo_phase(sock, f, 'emu')
    if verbose:
        print(f'[demo_sync] initial: rec={rec_phase} emu={emu_phase}')

    extra_rec = extra_emu = 0
    while rec_phase != emu_phase and (extra_rec + extra_emu) < max_extra_steps:
        # Step whichever side is "earlier" (smaller phase).
        if _phase_lt(rec_phase, emu_phase):
            _step(sock, f, 'rec')
            extra_rec += 1
            rec_phase = _read_demo_phase(sock, f, 'rec')
        else:
            _step(sock, f, 'emu')
            extra_emu += 1
            emu_phase = _read_demo_phase(sock, f, 'emu')

    synced = (rec_phase == emu_phase)
    if verbose:
        if synced:
            print(f'[demo_sync] synced: phase={rec_phase} '
                  f'(rec +{extra_rec}, emu +{extra_emu})')
        else:
            print(f'[demo_sync] FAILED to sync after '
                  f'{extra_rec + extra_emu} extra steps; '
                  f'rec={rec_phase} emu={emu_phase}')
    return {
        'rec_phase': rec_phase, 'emu_phase': emu_phase,
        'extra_steps_rec': extra_rec, 'extra_steps_emu': extra_emu,
        'synced': synced,
    }


def step_both_to_gm07_and_sync(sock, f, verbose: bool = False) -> dict:
    """Convenience: GM=07 sync + demo-phase sync in one call."""
    rs, es = step_both_to_gm07(sock, f)
    info = sync_demo_phase(sock, f, verbose=verbose)
    info['gm07_steps_rec'] = rs
    info['gm07_steps_emu'] = es
    return info
