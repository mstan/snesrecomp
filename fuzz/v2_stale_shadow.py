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

    1. For each snippet, decode its rom_hex with v2.decoder, lower with
       v2.lowering, emit C body via v2.codegen.emit_op.
    2. Wrap the emitted body in a tiny C harness that:
         - allocates a CpuState,
         - seeds A/X/Y/M/X-flag/D from snippet['init'],
         - calls the emitted body,
         - prints the final CpuState as JSON to stdout.
    3. Compile with cl.exe / link in cpu_state.c.
    4. Run, capture JSON, diff against snippet['expect'].

Run:
    python snesrecomp/fuzz/v2_stale_shadow.py
exits 0 if all snippets pass; non-zero with a per-snippet diff otherwise.
"""
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile

FUZZ = pathlib.Path(__file__).resolve().parent
REPO = FUZZ.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import snes65816 as s65   # noqa: E402  -- v1 decoder, used to verify rom_hex parses
from v2 import decoder, lowering, codegen, cfg as v2cfg  # noqa: E402
from v2.ir import Reg, IRBlock  # noqa: E402


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

C_HARNESS_PROLOGUE = r'''
/* v2 stale-shadow fuzz harness. Auto-generated. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;

/* Subset of CpuState matching cpu_state.h (no separate B field after
 * commit 84b359e). Stays in sync by structural correspondence; if the
 * real struct grows fields, harness still works because it only sets/
 * reads the named members. */
typedef struct CpuState {
    uint16 A;
    uint16 X;
    uint16 Y;
    uint16 S;
    uint16 D;
    uint8  DB;
    uint8  PB;
    uint8  P;
    uint8  m_flag;
    uint8  x_flag;
    uint8  emulation;
    uint8  _flag_N;
    uint8  _flag_V;
    uint8  _flag_Z;
    uint8  _flag_C;
    uint8  _flag_I;
    uint8  _flag_D;
    uint8 *ram;
} CpuState;

#define CPU_P_C 0x01u
#define CPU_P_Z 0x02u
#define CPU_P_I 0x04u
#define CPU_P_D 0x08u
#define CPU_P_X 0x10u
#define CPU_P_M 0x20u
#define CPU_P_V 0x40u
#define CPU_P_N 0x80u

static inline void cpu_p_to_mirrors(CpuState *cpu) {
    cpu->m_flag  = (cpu->P & CPU_P_M) ? 1 : 0;
    cpu->x_flag  = (cpu->P & CPU_P_X) ? 1 : 0;
    cpu->_flag_C = (cpu->P & CPU_P_C) ? 1 : 0;
    cpu->_flag_Z = (cpu->P & CPU_P_Z) ? 1 : 0;
    cpu->_flag_I = (cpu->P & CPU_P_I) ? 1 : 0;
    cpu->_flag_D = (cpu->P & CPU_P_D) ? 1 : 0;
    cpu->_flag_V = (cpu->P & CPU_P_V) ? 1 : 0;
    cpu->_flag_N = (cpu->P & CPU_P_N) ? 1 : 0;
}
static inline void cpu_mirrors_to_p(CpuState *cpu) {
    cpu->P = (uint8)(
        (cpu->m_flag  ? CPU_P_M : 0) |
        (cpu->x_flag  ? CPU_P_X : 0) |
        (cpu->_flag_C ? CPU_P_C : 0) |
        (cpu->_flag_Z ? CPU_P_Z : 0) |
        (cpu->_flag_I ? CPU_P_I : 0) |
        (cpu->_flag_D ? CPU_P_D : 0) |
        (cpu->_flag_V ? CPU_P_V : 0) |
        (cpu->_flag_N ? CPU_P_N : 0)
    );
}
static inline uint8 cpu_read_b(const CpuState *cpu) {
    return (uint8)((cpu->A >> 8) & 0xFF);
}

/* 128 KB WRAM. */
static uint8_t g_ram[0x20000];

/* cpu_read/cpu_write — same map as cpu_state.c. */
static int cpu_ram_offset(uint8 bank, uint16 addr) {
    if (bank == 0x7E) return (int)addr;
    if (bank == 0x7F) return 0x10000 + (int)addr;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)))
        return (int)addr;
    return -1;
}
static uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    return (off >= 0) ? cpu->ram[off] : 0xFF;
}
static uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000)
        return (uint16)cpu->ram[off] | ((uint16)cpu->ram[off+1] << 8);
    return 0xFFFF;
}
static void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) cpu->ram[off] = v;
}
static void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000) {
        cpu->ram[off]   = (uint8)(v & 0xFF);
        cpu->ram[off+1] = (uint8)(v >> 8);
    }
}

/* Stubs the v2 codegen sometimes emits even for fragment snippets. */
static void cpu_trace_event(CpuState *cpu, uint32 a, uint8 b, uint8 c, uint16 d) {
    (void)cpu; (void)a; (void)b; (void)c; (void)d;
}
static void cpu_trace_px_record(CpuState *cpu, uint32 a, uint8 b, uint8 c, uint8 d) {
    (void)cpu; (void)a; (void)b; (void)c; (void)d;
}
static void WatchdogCheck(void) {}

'''

C_HARNESS_EPILOGUE = r'''
int main(void) {
    int fail = 0;
    run_all(&fail);
    return fail;
}
'''


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
        out.append(f'    run_{sn["id"]}(&cpu);')
        # Emit one JSON line of post-state.
        out.append(f'    printf("{{\\"id\\":\\"{sn["id"]}\\","')
        out.append('           "\\"A\\":%u,\\"X\\":%u,\\"Y\\":%u,\\"D\\":%u,"')
        out.append('           "\\"m\\":%u,\\"x\\":%u,"')
        out.append('           "\\"_flag_N\\":%u,\\"_flag_Z\\":%u,'
                  '\\"_flag_C\\":%u,\\"_flag_V\\":%u,"')
        out.append('           "\\"wram_0x10\\":%u,\\"wram_0x11\\":%u}\\n",')
        out.append('           cpu.A, cpu.X, cpu.Y, cpu.D,')
        out.append('           cpu.m_flag, cpu.x_flag,')
        out.append('           cpu._flag_N, cpu._flag_Z, cpu._flag_C, cpu._flag_V,')
        out.append('           g_ram[0x10], g_ram[0x11]);')
    out.append('    (void)fail;')
    out.append('}')
    return '\n'.join(out)


def main() -> int:
    src = C_HARNESS_PROLOGUE + render_run_all(SNIPPETS) + C_HARNESS_EPILOGUE
    work = pathlib.Path(tempfile.mkdtemp(prefix='v2_stale_shadow_'))
    src_path = work / 'fuzz.c'
    exe_path = work / 'fuzz.exe'
    src_path.write_text(src, encoding='utf-8')

    # Locate cl.exe via vswhere or PATH; fall back to just trying.
    cl_candidates = [
        'cl.exe',
        r'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC',
    ]
    # Easiest: invoke a vcvars batch, then compile.
    bat = work / 'build.bat'
    bat.write_text(f'''@echo off
call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >NUL
cl /nologo /O2 /Fe:"{exe_path}" "{src_path}" >NUL
exit /b %ERRORLEVEL%
''', encoding='utf-8')
    rc = subprocess.run(['cmd', '/c', str(bat)], capture_output=True, text=True)
    if rc.returncode != 0:
        print('build failed:', file=sys.stderr)
        print(rc.stdout, file=sys.stderr)
        print(rc.stderr, file=sys.stderr)
        print(f'(source preserved at {src_path})', file=sys.stderr)
        return 2

    rc = subprocess.run([str(exe_path)], capture_output=True, text=True)
    if rc.returncode != 0:
        print(f'run failed (rc={rc.returncode}):', file=sys.stderr)
        print(rc.stdout, file=sys.stderr)
        print(rc.stderr, file=sys.stderr)
        return 3

    results = {}
    for line in rc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            print(f'bad output line: {line!r}', file=sys.stderr)
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
        print(f'\n(harness preserved at {work} for inspection)')
        return 1
    print('all snippets pass')
    return 0


if __name__ == '__main__':
    sys.exit(main())
