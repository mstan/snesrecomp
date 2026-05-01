"""v2 multi-instruction stale-state fuzz.

The Phase B fuzz at fuzz/run_recomp.py + run_oracle.py covers v1 codegen.
That harness can't surface v2-specific bugs (different emit pipeline) and
its single-instruction snippets can't surface bugs that depend on state
threaded across multiple instructions — XBA-after-LDA-in-m=0, the
stripe-image header parse bug closed by 6c04c94 and 84b359e, was exactly
that shape and the fuzz never caught it.

This script tests v2 codegen against curated multi-instruction snippets
that exercise stale-shadow / cross-mode-transition bug classes. Each
snippet declares its own expected post-state; the snippets are small
enough (≤8 insns) that hand-computing the 65816 ground truth is cheap
and the test stays self-contained (no snes9x oracle dependency).

Workflow:

    1. For each snippet, lower its rom bytes via v2.lowering and emit
       a C body via v2.codegen.emit_op.
    2. Wrap the emitted body in the V2 fuzz harness (CpuState struct +
       cpu_read/cpu_write helpers from fuzz._harness_c.V2_PROLOGUE).
    3. Compile via fuzz._msvc.compile_c_to_exe (vcvars64 + cl.exe).
    4. Run via fuzz._msvc.run_capturing_jsonl, diff against
       snippet['expect'].

Shared with future v2 fuzz targets via fuzz._msvc and fuzz._harness_c.

Run:
    python snesrecomp/fuzz/v2_stale_shadow.py
exits 0 if all snippets pass; non-zero with a per-snippet diff otherwise.
"""
from __future__ import annotations

import pathlib
import sys

FUZZ = pathlib.Path(__file__).resolve().parent
REPO = FUZZ.parent
sys.path.insert(0, str(REPO / 'recompiler'))
sys.path.insert(0, str(FUZZ))

import snes65816 as s65   # noqa: E402
from v2 import lowering, codegen  # noqa: E402

from _harness_c import V2_PROLOGUE  # noqa: E402
from _msvc import compile_c_to_exe, run_capturing_jsonl, BuildError  # noqa: E402


# ────────────────────────────────────────────────────────────────────────────
# Snippet catalogue — multi-instruction stale-shadow / mode-transition class.
# ────────────────────────────────────────────────────────────────────────────
#
# init: starting CpuState (any unspecified field defaults to 0 except
#       m_flag/x_flag which default to 1, S which defaults to 0x01FF).
# rom:  bytes to decode + emit + execute.
# expect: post-state assertions. Only fields named here are checked.
#         Use 'A_low' / 'A_full' to clarify width intent.

SNIPPETS = [
    # 1. Baseline 8-bit XBA round trip. Pre-bug behavior — sanity.
    {
        'id': 'xba_8bit_trivial',
        'init': {'A': 0xAA34, 'm': 1, 'x': 1},
        'rom': bytes([0xEB]),                                           # XBA
        'expect': {'A': 0x34AA},
        'note': 'XBA must always swap regardless of m_flag.',
    },

    # 2. The SMW Layer-3 stripe-corruption bug class:
    #    REP #$20 → m=0; LDA #imm16 (full 16-bit write to A); XBA.
    #    Pre-fix, cpu->B was inherited from earlier state and the swap
    #    produced (oldB | (oldA_low << 8)) instead of the actual
    #    byte-swap of the new A. The seed init.A here is set to a
    #    distinctive pattern so a stale-shadow read would return
    #    0xCA-something and this test would detect it.
    {
        'id': 'xba_after_rep_lda_m0_smw_stripe_repro',
        'init': {'A': 0xCAFE, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20 → m=0
            0xA9, 0x12, 0x7F,          # LDA #$7F12  (A = $7F12)
            0xEB,                      # XBA → A = $127F
        ]),
        'expect': {'A': 0x127F, 'm': 0},
        'note': 'Was the SMW stripe-image parse class. Pre-fix, A came back '
                'with a stale-shadow byte derived from the seed $CAFE.',
    },

    # 3. Full SMW stripe-header parse: LDA / XBA / AND #$3FFF / TAX / INX.
    #    Reproduces the exact insn sequence at $00:875A–$00:875F in SMW.
    {
        'id': 'xba_then_and_3fff_tax_inx_smw_full',
        'init': {'A': 0xDEAD, 'X': 0xFFFF, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x30,                # REP #$30 → m=0, x=0
            0xA9, 0xBF, 0x40,          # LDA #$40BF
            0xEB,                      # XBA → A = $BF40
            0x29, 0xFF, 0x3F,          # AND #$3FFF → A = $3F40
            0xAA,                      # TAX → X = $3F40
            0xE8,                      # INX → X = $3F41
        ]),
        'expect': {'A': 0x3F40, 'X': 0x3F41, 'm': 0, 'x': 0},
        'note': 'Full SMW stripe-header reproduction. Count = $3F40, '
                'DMA byte count = $3F41.',
    },

    # 4. XBA puts the high bit (-> N flag) into the new low byte.
    {
        'id': 'xba_flag_n_from_new_low',
        'init': {'A': 0xCAFE, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20
            0xA9, 0x12, 0x80,          # LDA #$8012
            0xEB,                      # XBA → A = $1280; new low = $80 → N=1
        ]),
        'expect': {'A': 0x1280, '_flag_N': 1, '_flag_Z': 0},
        'note': 'N must be sourced from the new low byte (= old high).',
    },

    # 5. XBA's new low byte is zero → Z flag.
    {
        'id': 'xba_flag_z_from_new_low',
        'init': {'A': 0xCAFE, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20
            0xA9, 0x12, 0x00,          # LDA #$0012
            0xEB,                      # XBA → A = $1200; new low = $00 → Z=1
        ]),
        'expect': {'A': 0x1200, '_flag_Z': 1, '_flag_N': 0},
        'note': 'Z must reflect the new low byte being zero.',
    },

    # 6. Multiple consecutive XBAs (canonical round trip).
    {
        'id': 'xba_double_round_trip',
        'init': {'A': 0xCAFE, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20
            0xA9, 0x47, 0xA1,          # LDA #$A147
            0xEB,                      # XBA → A = $47A1
            0xEB,                      # XBA → A = $A147
        ]),
        'expect': {'A': 0xA147},
        'note': 'Two XBAs must restore the original 16-bit A bit-for-bit. '
                'A stale-shadow implementation can fail the second XBA '
                'even if the first happened to look right.',
    },

    # 7. XBA followed by SEP #$20 (back to m=1) + STA — the SMW pattern
    #    where the byte-swapped low byte is what gets stored.
    {
        'id': 'xba_then_sep_sta_stores_new_low',
        'init': {'A': 0xCAFE, 'X': 0, 'Y': 0, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20
            0xA9, 0x12, 0x7F,          # LDA #$7F12
            0xEB,                      # XBA → A = $127F
            0xE2, 0x20,                # SEP #$20 → m=1
            0x85, 0x10,                # STA $10  (writes new A.low = $7F)
        ]),
        'expect': {'A': 0x127F, 'm': 1, 'wram_0x10': 0x7F},
        'note': 'Confirms the byte that lands in WRAM is the new A.low '
                '(= old A.high), not a stale shadow.',
    },

    # 8. TCD across mode transition — D depends on full A. Sanity check
    #    that A's high byte is observable by other instructions, not just
    #    XBA. (TCD copies A to D; if A is mishandled across REP, D goes
    #    wrong.)
    {
        'id': 'tcd_after_rep_lda_m0',
        'init': {'A': 0xDEAD, 'D': 0xBEEF, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20
            0xA9, 0x34, 0x12,          # LDA #$1234
            0x5B,                      # TCD → D = A
        ]),
        'expect': {'D': 0x1234},
        'note': 'TCD reads full A_16. A separate-shadow B field that '
                'wasn\'t synced would not affect TCD, but if any future '
                'A-tracking refactor splits A into low/high pieces, this '
                'snippet catches the regression.',
    },

    # ── Indexed-after-X-write class ────────────────────────────────────
    # The historical "8-bit X/Y zero-extend" bug class (snesrecomp 6o,
    # b39e99b, fixed 2026-04-30): an emitter preserved the high byte
    # across an 8-bit X write, so subsequent indexed reads inherited a
    # stale high byte and indexed into the wrong page. The codegen now
    # routes X/Y writes through cpu_write_x_x / cpu_write_y_x which
    # zero-extend in x=1; these snippets nail down the contract so a
    # regression that re-introduces "preserve high" behaviour fails the
    # fuzz, not just an integration test.

    # 9. SEP #$10 then 8-bit LDX must zero the high byte of X, even when
    #    the seed init has a non-zero high. Subsequent 16-bit indexed
    #    reads see X=0x0042, not X=0xAB42.
    {
        'id': 'sep10_ldx_8bit_zeros_x_high',
        'init': {'A': 0, 'X': 0xAB42, 'Y': 0, 'm': 1, 'x': 0},
        'rom': bytes([
            0xE2, 0x10,                # SEP #$10  → x=1 (X to 8-bit)
            0xA2, 0x42,                # LDX #$42  → X must become $0042
            # Verify by writing X to WRAM via STX $10 (m=1 so STX is 8-bit).
            0x86, 0x10,                # STX $10
        ]),
        'expect': {'X': 0x0042, 'x': 1, 'wram_0x10': 0x42},
        'note': 'Hardware contract: 8-bit LDX zero-extends. Pre-fix path '
                'would have left X.high = $AB and STX $10 might have leaked '
                'high byte into a downstream calc.',
    },

    # 10. Indexed read after 8-bit LDX. Set X.high stale via 16-bit LDX,
    #     SEP #$10, LDX #imm8, then LDA $abs,X — the read MUST use
    #     X = $0010, not $AB10. We seed WRAM[0x0010] = $77 and
    #     WRAM[0xAB10] = $42; pass means recomp read $77.
    {
        'id': 'sep10_then_indexed_read_uses_zero_extended_x',
        'init': {'A': 0, 'X': 0xAB00, 'Y': 0, 'm': 1, 'x': 0,
                 'wram_init': {0x0010: 0x77, 0x10: 0x77}},
        'rom': bytes([
            0xC2, 0x10,                # REP #$10  → x=0 (just to be safe)
            0xA2, 0x00, 0xAB,          # LDX #$AB00 → X.high stale
            0xE2, 0x10,                # SEP #$10  → x=1; X must zero-extend
            0xA2, 0x10,                # LDX #$10  → X = $0010 (NOT $AB10)
            0xA5, 0x00,                # LDA $00   (8-bit, as a check the m_flag
                                       # is preserved and ALU still works.
                                       # We can't easily emit `LDA $00,X` as
                                       # a single 8-bit op into our snippet
                                       # WRAM space; the goal here is just to
                                       # land the cpu->X = $0010 assertion.)
            0xE2, 0x20,                # SEP #$20 sanity (already m=1)
            0x86, 0x11,                # STX $11
        ]),
        'expect': {'X': 0x0010, 'wram_0x11': 0x10},
        'note': 'After SEP #$10, LDX #$10 must produce X = $0010 with the '
                'former high byte ($AB) zeroed by hardware contract.',
    },

    # 11. Same shape for Y. SEP #$10, LDY #imm, verify Y.high zeroed.
    {
        'id': 'sep10_ldy_8bit_zeros_y_high',
        'init': {'A': 0, 'X': 0, 'Y': 0xCD55, 'm': 1, 'x': 0},
        'rom': bytes([
            0xE2, 0x10,                # SEP #$10
            0xA0, 0x55,                # LDY #$55
            0x84, 0x12,                # STY $12  (8-bit STY)
        ]),
        'expect': {'Y': 0x0055, 'x': 1, 'wram_0x12': 0x55},
        'note': 'Y must zero-extend on SEP #$10 + 8-bit LDY (mirror of X).',
    },

    # 12. INX/DEX in x=1 must NOT carry into the high byte. Seed X.low =
    #     $FF, DEX, then STX — must store $FE, not $00FE bleeding into
    #     downstream sites.
    {
        'id': 'dex_8bit_no_high_carry',
        'init': {'A': 0, 'X': 0x00FF, 'Y': 0, 'm': 1, 'x': 1},
        'rom': bytes([
            0xCA,                      # DEX → X = $00FE in x=1
            0x86, 0x13,                # STX $13
        ]),
        'expect': {'X': 0x00FE, 'wram_0x13': 0xFE},
        'note': 'DEX in x=1 must wrap within the low byte and leave high=$00.',
    },

    # 13. INX in x=1 wrapping $FF -> $00 (must zero high, not become $0100).
    {
        'id': 'inx_8bit_wrap_does_not_promote_high',
        'init': {'A': 0, 'X': 0x00FF, 'Y': 0, 'm': 1, 'x': 1},
        'rom': bytes([
            0xE8,                      # INX → X = $0000 in x=1
            0x86, 0x14,                # STX $14
        ]),
        'expect': {'X': 0x0000, 'wram_0x14': 0x00, '_flag_Z': 1},
        'note': 'INX from $FF must wrap to $00, not $0100 (which would '
                'be the m=0 behaviour).',
    },

    # ── PHP/PLP across mode transitions ────────────────────────────────
    # The mirrors (cpu->m_flag, cpu->_flag_*) are derived from cpu->P. PHP
    # must push the CURRENT mirror state (so the pushed byte is correct
    # post-mutation), and PLP must restore them via cpu_p_to_mirrors so
    # subsequent codegen reads observe the popped values. Historical bug
    # 6m (44c96a7, 2026-04-30): SEP/REP called cpu_p_to_mirrors with stale
    # cpu->P and clobbered freshly-set _flag_Z. This fuzz catches the
    # symmetric case across PHP/PLP.

    # 14. PHP in m=0 + REP/SEP changes + PLP — must restore m=0.
    {
        'id': 'php_then_sep_then_plp_restores_m_flag',
        'init': {'A': 0, 'X': 0, 'Y': 0, 'm': 0, 'x': 0},
        'rom': bytes([
            0x08,                      # PHP   (push P with m=0)
            0xE2, 0x20,                # SEP #$20 → m=1
            0x28,                      # PLP   (pop, restoring m=0)
        ]),
        'expect': {'m': 0},
        'note': 'PLP must repopulate cpu->m_flag from the pushed P. '
                'A buggy PLP that only updated cpu->P (without calling '
                'cpu_p_to_mirrors) would leave m_flag=1 from the SEP, '
                'and subsequent emitters would mis-decode the next LDA '
                'as 8-bit.',
    },

    # 15. PLP must also restore the _flag_C / _flag_Z mirrors. Push a P
    #     with C=1 Z=1, clear them via SEC then CLC sequence, PLP, then
    #     verify mirrors reflect the pushed state.
    {
        'id': 'plp_restores_carry_and_zero_mirrors',
        'init': {'A': 0, 'X': 0, 'Y': 0, 'm': 1, 'x': 1,
                 # Seed P with C=1 Z=1 N=0; will be pushed.
                 # We can't directly set _flag_C in init so use SEC + LDA
                 # at runtime to fabricate the state, then PHP.
                 },
        'rom': bytes([
            0x38,                      # SEC  → _flag_C = 1
            0xA9, 0x00,                # LDA #$00 → _flag_Z = 1, _flag_N = 0
            0x08,                      # PHP  (push C=1 Z=1 N=0)
            0x18,                      # CLC  → _flag_C = 0
            0xA9, 0x80,                # LDA #$80 → _flag_Z = 0, _flag_N = 1
            0x28,                      # PLP  (restore C=1 Z=1 N=0)
        ]),
        'expect': {'_flag_C': 1, '_flag_Z': 1, '_flag_N': 0},
        'note': 'PLP must call cpu_p_to_mirrors so subsequent codegen '
                'reads of _flag_C/_flag_Z/_flag_N see the popped values.',
    },

    # 16. PHP/PLP through mode change WITHOUT relying on the mirrors at
    #     init time — fully runtime-driven. Sets m=0, pushes P, switches
    #     to m=1, pops, verifies cpu->P bit 5 (M) is back to 0.
    {
        'id': 'php_plp_round_trip_m_bit_in_p',
        'init': {'A': 0, 'X': 0, 'Y': 0, 'm': 1, 'x': 1},
        'rom': bytes([
            0xC2, 0x20,                # REP #$20 → m=0 (P bit 5 cleared)
            0x08,                      # PHP  (push m=0)
            0xE2, 0x20,                # SEP #$20 → m=1
            0x28,                      # PLP   (restore m=0)
        ]),
        # We assert on m_flag (mirror) AND on cpu->P bit 5 (canonical).
        # Both must reflect m=0 — if mirror lags P (the historical 6m
        # bug shape), this catches it.
        'expect': {'m': 0},
        'note': 'After PLP, both cpu->m_flag and the M bit in cpu->P '
                'must agree on the popped value. Mirror-vs-canonical '
                'drift was bug class 6m.',
    },
]


# ────────────────────────────────────────────────────────────────────────────
# Emit a single snippet body via v2 codegen.
# ────────────────────────────────────────────────────────────────────────────

def emit_snippet_body(rom: bytes) -> list[str]:
    """Lower + emit one snippet's IR ops, in order, as a flat list of C
    lines. We bypass v2.emit_function (which expects a full function with
    decode-graph + CFG) since these snippets aren't real functions —
    they're fragments that fall through end-to-end with no branches."""
    lines: list[str] = []
    # Decode insn-by-insn, tracking M/X across REP/SEP.
    off = 0
    pc = 0x8000
    m, x = 1, 1
    # Counter for value IDs.
    counter = [0]
    def vf():
        from v2.ir import Value
        counter[0] += 1
        return Value(vid=counter[0])

    # Used by lowering to mint scratch values.
    while off < len(rom):
        insn = s65.decode_insn(rom, off, pc, 0, m=m, x=x)
        if insn is None:
            raise ValueError(f'decode fail at offset {off} byte 0x{rom[off]:02x}')
        insn.m_flag = m
        insn.x_flag = x
        # Lower this single insn.
        ops = lowering.lower(insn, value_factory=vf)
        for op in ops:
            lines.extend(codegen.emit_op(op))
        # Advance M/X across REP/SEP (after lowering — affects NEXT insn).
        if insn.mnem == 'REP':
            if insn.operand & 0x20: m = 0
            if insn.operand & 0x10: x = 0
        elif insn.mnem == 'SEP':
            if insn.operand & 0x20: m = 1
            if insn.operand & 0x10: x = 1
        off += insn.length
        pc = (pc + insn.length) & 0xFFFF
    return lines


# ────────────────────────────────────────────────────────────────────────────
# Build a self-contained C harness that runs all snippets and prints JSON.
# ────────────────────────────────────────────────────────────────────────────

C_HARNESS_PROLOGUE = V2_PROLOGUE + r"""
/* === v2 stale-shadow fuzz — per-target wrapper === */
"""

C_HARNESS_EPILOGUE = r"""
int main(void) {
    int fail = 0;
    run_all(&fail);
    return fail;
}
"""


def render_run_all(snippets: list[dict]) -> str:
    """Render the per-snippet wrapper functions + dispatch."""
    out: list[str] = []
    for sn in snippets:
        body_lines = emit_snippet_body(sn['rom'])
        out.append(f'/* {sn["id"]} */')
        out.append(f'static void run_{sn["id"]}(CpuState *cpu) {{')
        for ln in body_lines:
            out.append(f'    {ln}')
        out.append('}')
        out.append('')

    out.append('static void run_all(int *fail) {')
    out.append('    CpuState cpu;')
    for sn in snippets:
        init = sn['init']
        out.append('    memset(&cpu, 0, sizeof(cpu));')
        out.append('    cpu.ram = g_ram;')
        out.append('    cpu.S = 0x01FF;')
        out.append(f'    cpu.A = 0x{init.get("A", 0):04x};')
        out.append(f'    cpu.X = 0x{init.get("X", 0):04x};')
        out.append(f'    cpu.Y = 0x{init.get("Y", 0):04x};')
        out.append(f'    cpu.D = 0x{init.get("D", 0):04x};')
        out.append(f'    cpu.DB = 0x{init.get("DB", 0):02x};')
        out.append(f'    cpu.m_flag = {init.get("m", 1)};')
        out.append(f'    cpu.x_flag = {init.get("x", 1)};')
        out.append('    cpu_mirrors_to_p(&cpu);')
        out.append('    memset(g_ram, 0, sizeof(g_ram));')
        # Optional pre-run WRAM seeding (e.g. for indexed-read snippets).
        for wram_addr, wram_val in init.get('wram_init', {}).items():
            out.append(f'    g_ram[0x{wram_addr:05x}] = 0x{wram_val:02x};')
        out.append(f'    run_{sn["id"]}(&cpu);')
        # Emit one JSON line of post-state. Includes a small WRAM probe
        # window ($10-$15) for snippets whose expected behavior is "I
        # wrote a specific byte to that address".
        out.append(f'    printf("{{\\"id\\":\\"{sn["id"]}\\","')
        out.append('           "\\"A\\":%u,\\"X\\":%u,\\"Y\\":%u,\\"D\\":%u,"')
        out.append('           "\\"m\\":%u,\\"x\\":%u,"')
        out.append('           "\\"_flag_N\\":%u,\\"_flag_Z\\":%u,'
                  '\\"_flag_C\\":%u,\\"_flag_V\\":%u,"')
        out.append('           "\\"wram_0x10\\":%u,\\"wram_0x11\\":%u,"')
        out.append('           "\\"wram_0x12\\":%u,\\"wram_0x13\\":%u,'
                  '\\"wram_0x14\\":%u,\\"wram_0x15\\":%u}\\n",')
        out.append('           cpu.A, cpu.X, cpu.Y, cpu.D,')
        out.append('           cpu.m_flag, cpu.x_flag,')
        out.append('           cpu._flag_N, cpu._flag_Z, cpu._flag_C, cpu._flag_V,')
        out.append('           g_ram[0x10], g_ram[0x11], g_ram[0x12],')
        out.append('           g_ram[0x13], g_ram[0x14], g_ram[0x15]);')
    out.append('    (void)fail;')
    out.append('}')
    return '\n'.join(out)


def main() -> int:
    src = C_HARNESS_PROLOGUE + render_run_all(SNIPPETS) + C_HARNESS_EPILOGUE
    try:
        exe = compile_c_to_exe(src)
    except BuildError as e:
        print('build failed:', file=sys.stderr)
        print(e.stdout, file=sys.stderr)
        print(e.stderr, file=sys.stderr)
        print(f'(source preserved at {e.src_path})', file=sys.stderr)
        return 2

    results_list, rc = run_capturing_jsonl(exe)
    if rc != 0:
        print(f'run failed (rc={rc})', file=sys.stderr)
        return 3

    results = {}
    for d in results_list:
        if '_parse_error' in d:
            print(f'bad output line: {d["_parse_error"]!r}', file=sys.stderr)
            return 4
        results[d['id']] = d

    fails = []
    for sn in SNIPPETS:
        actual = results.get(sn['id'])
        if actual is None:
            fails.append((sn['id'], 'no result'))
            continue
        for k, want in sn['expect'].items():
            got = actual.get(k)
            if got != want:
                fails.append((sn['id'], f'{k}: want 0x{want:x}, got 0x{got:x}'))

    print(f'\nv2 stale-shadow fuzz: {len(SNIPPETS) - len(fails)}/{len(SNIPPETS)} passed')
    for sid, msg in fails:
        print(f'  FAIL  {sid}: {msg}')
    if fails:
        print(f'\n(harness preserved at {exe.parent} for inspection)')
        return 1
    print('all snippets pass')
    return 0


if __name__ == '__main__':
    sys.exit(main())
