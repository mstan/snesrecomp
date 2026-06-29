"""Pin the v2 decoder's runtime-pointer JSR (abs,X) recovery (2026-06-21).

A reachable `JSR (abs,X)` whose pointer-table base is in WRAM/DP/low-RAM
($0000-$1FFF) is a genuine per-object runtime function-pointer dispatch —
SM's enemy/PLM/eproj instruction-list interpreters call
`JSR ($0FA8/$0FAE/$0FB0/$0FB2,X)` where $0FAx holds a per-object handler
pointer written at runtime. The target is a WRAM value that cannot be
statically enumerated, so the decoder marks the insn `dispatch_runtime` and
PRESERVES the fall-through (a JSR returns to the next instruction); codegen
routes it through cpu_dispatch_call_pc.

The discriminator is the operand range. A phantom `JSR (abs,X)` decoded from
garbage past an RTS has a ROM-range operand (>= $2000; see
test_decoder_smc_phantom_suppression's $EA1D phantom) and stays SUPPRESSED —
a function-pointer table never lives in PPU/APU registers ($2000-$5FFF) or
ROM ($8000+).

These tests pin:
  1. A WRAM-operand JSR (abs,X) is marked dispatch_runtime and KEEPS its
     fall-through edge (the post-JSR RTS is decoded).
  2. A ROM-operand JSR (abs,X) is NOT dispatch_runtime and its fall-through
     stays severed (unchanged suppression behaviour).
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function  # noqa: E402
from snes65816 import INDIR_X  # noqa: E402


def _jsr_indir_x_key(graph, pc):
    for k in graph.insns:
        if (k.pc == pc and graph.insns[k].insn.mnem == 'JSR'
                and graph.insns[k].insn.mode == INDIR_X):
            return k
    return None


def test_wram_operand_jsr_indir_x_is_runtime_dispatch():
    """`FC A8 0F 60` = JSR ($0FA8,X) ; RTS entered at $8000 (M=1, X=1).

    Operand $0FA8 < $2000 -> dispatch_runtime, fall-through preserved, so
    the RTS at $8003 IS decoded."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xFC, 0xA8, 0x0F, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    k = _jsr_indir_x_key(graph, 0x008000)
    assert k is not None, "expected JSR (abs,X) decoded at $8000"
    di = graph.insns[k]
    assert getattr(di.insn, 'dispatch_runtime', False) is True, (
        "WRAM-operand JSR (abs,X) must be marked dispatch_runtime so codegen "
        "routes it through cpu_dispatch_call_pc instead of suppressing it."
    )
    assert di.successors, (
        "dispatch_runtime JSR (abs,X) must PRESERVE its fall-through edge — "
        f"a JSR returns to the next instruction; got {di.successors}."
    )
    assert any(sk.pc == 0x008003 for sk in di.successors), (
        "fall-through must reach the post-JSR insn at $8003 (the RTS)."
    )
    assert any(k2.pc == 0x008003 for k2 in graph.insns), (
        "the post-JSR RTS at $8003 must be decoded into the graph."
    )


def test_rom_operand_jsr_indir_x_stays_suppressed():
    """`FC 1D EA 60` = JSR ($EA1D,X) ; RTS entered at $8000 (M=1, X=1).

    Operand $EA1D >= $2000 -> NOT a runtime WRAM pointer; the unauthorised
    suppression still severs the fall-through ($8003 not decoded)."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xFC, 0x1D, 0xEA, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    k = _jsr_indir_x_key(graph, 0x008000)
    assert k is not None, "expected JSR (abs,X) decoded at $8000"
    di = graph.insns[k]
    assert getattr(di.insn, 'dispatch_runtime', False) is False, (
        "ROM-operand JSR (abs,X) must NOT be treated as a runtime WRAM "
        "pointer dispatch — that is the phantom-suppression discriminator."
    )
    assert di.successors == [], (
        "unauthorised ROM-operand JSR (abs,X) must keep its fall-through "
        f"severed; got {di.successors}."
    )
    assert not any(k2.pc == 0x008003 for k2 in graph.insns), (
        "the byte at $8003 must NOT be decoded along the suppressed path."
    )
