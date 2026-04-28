"""Real-ROM regression: GrassObjXX_DiagonalLedge JMP back-edge phi.

This is the canonical Issue B (2026-04-26) failure case. The function at
$0DB7AA contains an inner block at $0DB823 that ends with `JMP $0DB836`,
where $0DB836 is the outer DEX-BNE loop header and was emitted EARLIER
in the codegen (it's a back-edge in emission order even though forward
in PC order). The inner block reloads X via `LDX $1` (=8 in the attract
demo), then JMPs without syncing X into label_b836's captured X var.
The next outer-loop iteration's DEX wraps 0->0xFF, looping ~256 times
instead of 7-8, over-writing valid Map16 tiles. Visible symptom:
Mario sinks 1 block under in the attract demo.

Fix (recomp.py:5374): _emit_jmp ABS path now calls _emit_backedge_phi
before emitting the goto, mirroring the BRA back-edge path.

This test invokes the real recomp on bank 0D of the real ROM and
greps the emitted DiagonalLedge body for the phi-sync line. Skipped
when the ROM/cfg aren't present.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


def _paths():
    framework_root = pathlib.Path(__file__).absolute().parent.parent
    game_root = framework_root.parent
    rom = game_root / 'smw.sfc'
    cfg = game_root / 'recomp' / 'bank0d.cfg'
    recomp = framework_root / 'recompiler' / 'recomp.py'
    if not (rom.exists() and cfg.exists() and recomp.exists()):
        return None
    return rom, cfg, recomp


def _gen_bank_0d():
    paths = _paths()
    if paths is None:
        return None
    rom, cfg, recomp = paths
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / 'bank0d.c'
        r = subprocess.run(
            [sys.executable, str(recomp), str(rom), str(cfg),
             '--reverse-debug', '-o', str(out)],
            capture_output=True, text=True, timeout=180,
        )
        assert r.returncode == 0, f'recomp failed: {r.stderr}'
        return out.read_text()


def _extract_function(src: str, name: str) -> str:
    """Return the body lines of the function `name` (between the opening
    brace and the matching closing brace at column 0)."""
    # Match e.g. `void GrassObjXX_DiagonalLedge_LeftFacingDiagonalLedgeEntry(uint8 k) {  // 0db7aa`
    m = re.search(rf'^\S[^\n]*\b{re.escape(name)}\b[^\n]*\{{[^\n]*$',
                  src, re.MULTILINE)
    assert m, f'function {name!r} not found in source'
    start = m.end()
    end = src.find('\n}\n', start)
    assert end != -1, f'function {name!r} body close not found'
    return src[start:end]


def test_diagonal_ledge_b823_jmp_syncs_x_to_label_b836():
    """The inner block at $0DB823 (label_b823) reloads X via LDX $1
    (emitted as `vN = g_ram[0x1];` at $0DB847), then JMPs back to
    label_b836. The fix must emit `<label_b836's X var> = <current X>;`
    immediately before `goto label_b836;`.
    """
    src = _gen_bank_0d()
    if src is None:
        return  # ROM/cfg not available — skip
    body = _extract_function(
        src, 'GrassObjXX_DiagonalLedge_LeftFacingDiagonalLedgeEntry')
    # Find the b823 block (label_b823:; through the JMP-back goto).
    m = re.search(
        r'label_b823:;(.*?)goto label_b836;', body, re.DOTALL)
    assert m, (
        'expected label_b823:; ... goto label_b836; in DiagonalLedge body. '
        'gen output may have changed shape.\nBody:\n' + body
    )
    block = m.group(1)
    # Identify the X var captured at label_b836 by inspecting its
    # RDB_BLOCK_HOOK call (`RDB_BLOCK_HOOK(0x0db836, A, X, Y)`).
    hook_m = re.search(
        r'RDB_BLOCK_HOOK\(0x0db836,\s*\(uint32_t\)\(([^)]+)\),\s*'
        r'\(uint32_t\)\(([^)]+)\),\s*\(uint32_t\)\(([^)]+)\)\)',
        body)
    assert hook_m, 'expected RDB_BLOCK_HOOK at label_b836'
    label_x_var = hook_m.group(2).strip()
    # The current X at the JMP site is the var holding `g_ram[0x1]` —
    # find that allocation inside the b836-after-block (between
    # label_b836:; and label_b823:;).
    ldx_m = re.search(
        r'label_b836:;(.*?)label_b823:;',
        body, re.DOTALL)
    assert ldx_m, 'expected outer label_b836 ... label_b823 ordering'
    ldx_block = ldx_m.group(1)
    cur_x_m = re.search(r'(\w+)\s*=\s*g_ram\[0x1\]\s*;', ldx_block)
    assert cur_x_m, (
        'expected `vN = g_ram[0x1];` in label_b836 block (LDX $1). '
        'Block:\n' + ldx_block
    )
    cur_x_var = cur_x_m.group(1)
    # The fix: a line `<label_x_var> = <cur_x_var>;` must appear in the
    # b823 block, before the JMP-back goto.
    expected = f'{label_x_var} = {cur_x_var};'
    assert expected in block, (
        f'MISSING phi sync `{expected}` before `goto label_b836;` in '
        f'label_b823 block. This is the Issue B bug.\nb823 block:\n{block}'
    )
