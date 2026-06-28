#!/usr/bin/env python3
"""gen_ops.py — emit single-opcode recompiler functions for the codegen-vs-
interp816 differential harness (Axis 1, 65816 instruction semantics).

For each opcode test we use the REAL v2 emitter to translate `<opcode operands>
RTS` at $00:8000 into a C function, exactly as a recompiled game would get it.
The harness (cpu_diff.c) then runs that function and one interp816 step from an
identical CPU state over many randomized inputs and diffs the result.

Writes gen_ops.c (functions + a g_ops[] table). Run from the engine worktree:
  python tests/cpu_diff/gen_ops.py
"""
import os, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "v2"))
sys.path.insert(0, os.path.join(ROOT, "recompiler"))
from _helpers import make_lorom_bank0          # noqa: E402
from v2.emit_function import emit_function       # noqa: E402

RTS = 0x60
# Representative immediates to exercise ALU edge cases (carry/overflow/sign/zero).
IMM8 = [0x00, 0x01, 0x7F, 0x80, 0xFF, 0x40]

# (label, opcode, has_imm)  — immediate ALU/compare ops (mode IMM).
IMM_OPS = [
    ("lda", 0xA9, True), ("adc", 0x69, True), ("sbc", 0xE9, True),
    ("and", 0x29, True), ("ora", 0x09, True), ("eor", 0x49, True),
    ("cmp", 0xC9, True), ("bit", 0x89, True),
]
# index-immediate compares/loads (width = X flag)
IMMX_OPS = [("ldx", 0xA2), ("ldy", 0xA0), ("cpx", 0xE0), ("cpy", 0xC0)]
# implied / accumulator / transfer ops (no operand). width noted per op.
IMPLIED = [
    ("clc", 0x18), ("sec", 0x38), ("cld", 0xD8), ("sed", 0xF8), ("clv", 0xB8),
    ("asl_a", 0x0A), ("lsr_a", 0x4A), ("rol_a", 0x2A), ("ror_a", 0x6A),
    ("inc_a", 0x1A), ("dec_a", 0x3A),
    ("tax", 0xAA), ("tay", 0xA8), ("txa", 0x8A), ("tya", 0x98),
    ("tsx", 0xBA), ("txs", 0x9A), ("txy", 0x9B), ("tyx", 0xBB),
    ("tcd", 0x5B), ("tdc", 0x7B), ("tcs", 0x1B), ("tsc", 0x3B),
    ("inx", 0xE8), ("iny", 0xC8), ("dex", 0xCA), ("dey", 0x88),
    ("xba", 0xEB),
]

funcs = []      # emitted C source blocks
table = []      # (cname, codebytes, m, x, wmem)


def emit(label, code, m, x, wmem=0, idx=0, ind=0):
    fn = f"op_{label}"
    rom = make_lorom_bank0({0x8000: bytes(code) + bytes([RTS])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=m, entry_x=x,
                        func_name=fn)
    cname = f"{fn}_M{m}X{x}"
    if cname not in [t[0] for t in table]:
        funcs.append(src)
        table.append((cname, code, m, x, wmem, idx, ind))


for label, op, _ in IMM_OPS:
    for imm in IMM8:
        emit(f"{label}_{imm:02x}_m1", [op, imm], 1, 1)            # 8-bit A
        emit(f"{label}_{imm:02x}_lo_m0", [op, imm, 0x00], 0, 1)   # 16-bit A
        emit(f"{label}_{imm:02x}_hi_m0", [op, 0x00, imm], 0, 1)
for label, op in IMMX_OPS:
    for imm in IMM8:
        emit(f"{label}_{imm:02x}_x1", [op, imm], 1, 1)            # 8-bit index
        emit(f"{label}_{imm:02x}_lo_x0", [op, imm, 0x00], 1, 0)   # 16-bit index
# ── memory-addressing modes: dp ($10) and abs ($0040), DB in {0,1} at runtime ──
# kind: 'load' (reads mem, A-width), 'store', 'rmw', 'loadx'/'storex' (X-width)
DP, ABS = 0x10, 0x0040
MEM = [
    # A-width loads / ALU
    ("lda", 0xA5, 0xAD, 'load'),  ("adc", 0x65, 0x6D, 'load'),
    ("sbc", 0xE5, 0xED, 'load'),  ("and", 0x25, 0x2D, 'load'),
    ("ora", 0x05, 0x0D, 'load'),  ("eor", 0x45, 0x4D, 'load'),
    ("cmp", 0xC5, 0xCD, 'load'),
    # A-width stores
    ("sta", 0x85, 0x8D, 'store'), ("stz", 0x64, 0x9C, 'store'),
    # RMW (A-width on memory)
    ("inc", 0xE6, 0xEE, 'rmw'),   ("dec", 0xC6, 0xCE, 'rmw'),
    ("asl", 0x06, 0x0E, 'rmw'),   ("lsr", 0x46, 0x4E, 'rmw'),
    ("rol", 0x26, 0x2E, 'rmw'),   ("ror", 0x66, 0x6E, 'rmw'),
    # X-width loads/stores/compares (index-width)
    ("ldx", 0xA6, 0xAE, 'loadx'), ("ldy", 0xA4, 0xAC, 'loadx'),
    ("stx", 0x86, 0x8E, 'storex'),("sty", 0x84, 0x8C, 'storex'),
    ("cpx", 0xE4, 0xEC, 'loadx'), ("cpy", 0xC4, 0xCC, 'loadx'),
]
for label, dpop, absop, kind in MEM:
    wmem = 1 if kind in ('store', 'rmw', 'storex') else 0
    # width flags: A-width ops vary m (x=1); X-width ops vary x (m=1)
    widths = [(1, 1), (0, 1)] if kind in ('load', 'store', 'rmw') else [(1, 1), (1, 0)]
    for m, x in widths:
        emit(f"{label}_dp", [dpop, DP], m, x, wmem)
        emit(f"{label}_abs", [absop, ABS & 0xff, ABS >> 8], m, x, wmem)

# ── indexed addressing: abs,X / abs,Y / dp,X (index bounded at runtime) ──
# (label, dpx_op, absx_op, absy_op, kind)  — None where the mode doesn't exist
IDX = [
    ("lda", 0xB5, 0xBD, 0xB9, 'load'),  ("adc", 0x75, 0x7D, 0x79, 'load'),
    ("sbc", 0xF5, 0xFD, 0xF9, 'load'),  ("and", 0x35, 0x3D, 0x39, 'load'),
    ("ora", 0x15, 0x1D, 0x19, 'load'),  ("eor", 0x55, 0x5D, 0x59, 'load'),
    ("cmp", 0xD5, 0xDD, 0xD9, 'load'),
    ("sta", 0x95, 0x9D, 0x99, 'store'),
    ("inc", 0xF6, 0xFE, None,  'rmw'),  ("dec", 0xD6, 0xDE, None,  'rmw'),
    ("asl", 0x16, 0x1E, None,  'rmw'),  ("lsr", 0x56, 0x5E, None,  'rmw'),
    ("rol", 0x36, 0x3E, None,  'rmw'),  ("ror", 0x76, 0x7E, None,  'rmw'),
]
for label, dpx, absx, absy, kind in IDX:
    wmem = 1 if kind in ('store', 'rmw') else 0
    for m, x in ((1, 1), (0, 1)):
        if dpx is not None:
            emit(f"{label}_dpx", [dpx, DP], m, x, wmem, idx=1)
        if absx is not None:
            emit(f"{label}_absx", [absx, ABS & 0xff, ABS >> 8], m, x, wmem, idx=1)
        if absy is not None:
            emit(f"{label}_absy", [absy, ABS & 0xff, ABS >> 8], m, x, wmem, idx=1)

# ── indirect addressing: (dp,X) (dp) (dp),Y [dp] [dp],Y — pointer planted ──
# (label, dpx, dp, dpy, ldp, ldpy)  opcodes per ALU op
INDIR = [
    ("ora", 0x01, 0x12, 0x11, 0x07, 0x17), ("and", 0x21, 0x32, 0x31, 0x27, 0x37),
    ("eor", 0x41, 0x52, 0x51, 0x47, 0x57), ("adc", 0x61, 0x72, 0x71, 0x67, 0x77),
    ("sta", 0x81, 0x92, 0x91, 0x87, 0x97), ("lda", 0xA1, 0xB2, 0xB1, 0xA7, 0xB7),
    ("cmp", 0xC1, 0xD2, 0xD1, 0xC7, 0xD7), ("sbc", 0xE1, 0xF2, 0xF1, 0xE7, 0xF7),
]
for label, dpx, dp, dpy, ldp, ldpy in INDIR:
    wmem = 1 if label == "sta" else 0
    for m, x in ((1, 1), (0, 1)):
        emit(f"{label}_indx", [dpx, DP], m, x, wmem, idx=1, ind=3)  # (dp,X)
        emit(f"{label}_ind",  [dp,  DP], m, x, wmem, ind=1)         # (dp)
        emit(f"{label}_indy", [dpy, DP], m, x, wmem, idx=1, ind=1)  # (dp),Y
        emit(f"{label}_lind", [ldp, DP], m, x, wmem, ind=2)         # [dp]
        emit(f"{label}_lindy",[ldpy,DP], m, x, wmem, idx=1, ind=2)  # [dp],Y

# ── stack push/pull (operand-less). Pushes write the stack (wmem=1); the
# harness's S-=2 RTS-undo recovers the push/pull effect on S. ──
STACK = [
    ("pha", 0x48, 1), ("pla", 0x68, 0), ("phx", 0xDA, 1), ("plx", 0xFA, 0),
    ("phy", 0x5A, 1), ("ply", 0x7A, 0), ("php", 0x08, 1), ("plp", 0x28, 0),
    ("phb", 0x8B, 1), ("plb", 0xAB, 0), ("phd", 0x0B, 1), ("pld", 0x2B, 0),
    ("phk", 0x4B, 1),
]
for label, op, push in STACK:
    for m, x in ((1, 1), (0, 0)):
        emit(f"{label}", [op], m, x, wmem=push)

for label, op in IMPLIED:
    emit(f"{label}_m1", [op], 1, 1)
    # also a 16-bit-width variant for the width-sensitive ops
    if label in ("asl_a", "lsr_a", "rol_a", "ror_a", "inc_a", "dec_a",
                 "tax", "tay", "txa", "tya", "tcd", "tdc", "tcs", "tsc",
                 "inx", "iny", "dex", "dey", "xba"):
        emit(f"{label}_m0", [op], 0, 0)

out = os.path.join(os.path.dirname(__file__), "gen_ops.c")
with open(out, "w", newline="\n") as f:
    f.write('/* GENERATED by tests/cpu_diff/gen_ops.py — single-opcode recomp '
            'functions + table. */\n')
    f.write('#include "common_cpu_infra.h"\n#include "cpu_trace.h"\n')
    f.write('#include "cpu_diff.h"\n\n')
    for s in funcs:
        f.write(s)
        f.write("\n")
    f.write(f"\nconst OpTest g_ops[] = {{\n")
    for cname, code, m, x, wmem, idx, ind in table:
        cb = ",".join(f"0x{b:02x}" for b in code)
        f.write(f'  {{"{cname}", {{{cb}}}, {len(code)}, {cname}, {m}, {x}, {wmem}, {idx}, {ind}}},\n')
    f.write("};\n")
    f.write(f"const int g_nops = {len(table)};\n")
print(f"wrote {out}: {len(table)} opcode variants")
