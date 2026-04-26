"""Regen idempotency: two consecutive regens of the same bank/cfg
must produce byte-identical output.

Catches Python hash-randomization leaking into the emitter via set/
dict iteration that drives ordered output (variable declarations,
diagnostic-comment lists, etc.). Surfaced 2026-04-26 by the A/B
test that toggled the dispatch-extent reorder: even within a single
bank, consecutive regens diffed on `stk_XX` declaration order and
the "RomPtr with invalid banks" comment list.

Bank 01 is chosen because it exercises both code paths: it has the
longest gen file (all auto-promoted thunks land here), the most
`stk_XX` references in any single function (auto_01_8636), and a
diagnostic comment with multiple invalid banks. If bank 01 is
deterministic, the rest follow.

Skipped when ROM/cfg aren't present in the parent game checkout.
"""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile


def _paths():
    framework_root = pathlib.Path(__file__).absolute().parent.parent
    game_root = framework_root.parent
    rom = game_root / 'smw.sfc'
    cfg = game_root / 'recomp' / 'bank01.cfg'
    recomp = framework_root / 'recompiler' / 'recomp.py'
    if not (rom.exists() and cfg.exists() and recomp.exists()):
        return None
    return rom, cfg, recomp


def test_bank01_regen_byte_identical_twice():
    paths = _paths()
    if paths is None:
        return
    rom, cfg, recomp = paths
    with tempfile.TemporaryDirectory() as td:
        out_a = pathlib.Path(td) / 'bank01_a.c'
        out_b = pathlib.Path(td) / 'bank01_b.c'
        for out in (out_a, out_b):
            r = subprocess.run(
                [sys.executable, str(recomp), str(rom), str(cfg),
                 '--reverse-debug', '-o', str(out)],
                capture_output=True, text=True, timeout=120,
            )
            assert r.returncode == 0, f'recomp failed: {r.stderr}'
            assert out.exists(), f'no output: {out}'
        a = out_a.read_bytes()
        b = out_b.read_bytes()
        if a != b:
            # Find the first divergence offset for a useful diagnostic.
            n = min(len(a), len(b))
            first = next((i for i in range(n) if a[i] != b[i]), n)
            ctx_lo = max(0, first - 40)
            ctx_hi = min(n, first + 40)
            assert False, (
                f'bank01 regen non-deterministic: byte {first} differs '
                f'(len_a={len(a)} len_b={len(b)})\n'
                f'  a[{ctx_lo}:{ctx_hi}]={a[ctx_lo:ctx_hi]!r}\n'
                f'  b[{ctx_lo}:{ctx_hi}]={b[ctx_lo:ctx_hi]!r}'
            )
