"""Pin v2 decoder constant-Z fold behaviour.

A static recomp can't see CPU flags at decode time — but for the
narrow case `LD* #imm` immediately followed by `BEQ`/`BNE` in the
same basic block, Z is statically determined by the immediate. The
decoder rewrites the branch to a single live edge and prunes the
dead-edge insns from the graph.

This is a real ROM-fact analysis (the only path the CPU can take
given the immediate) — NOT a cfg hint that hides uncertainty.

Scope, deliberately narrow:
  * Predecessor must be LDA, LDX, or LDY in IMM addressing mode.
  * Width follows m for LDA, x for LDX/LDY (entry mode of the load,
    which is the mode under which decode_insn read its operand).
  * Predecessor must be the branch's only graph predecessor AND the
    branch must be the predecessor's only successor (no other edge
    can land on the load between it and the branch).
  * Only Z-flag branches: BEQ, BNE.

Anything else (N/V/C, ALU, SEP/REP/PLP, label-crossing) is OUT.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function  # noqa: E402


def _pcs(graph):
    return {k.pc & 0xFFFF for k in graph.insns}


def _insn_at(graph, pc16):
    """Return the (single) DecodedInsn at this 16-bit PC."""
    for k, di in graph.insns.items():
        if (k.pc & 0xFFFF) == pc16:
            return di
    return None


def _succ_pcs(graph, pc16):
    di = _insn_at(graph, pc16)
    return [s.pc & 0xFFFF for s in di.successors]


# ── Z=0 (LDX #$01) + BNE → unconditional jump to taken target ───────

def test_ldx_imm_nonzero_then_bne_keeps_only_taken_edge():
    # LDX #$01 ; BNE +4 (skip 4 bytes) ; RTS ; ... ; LDA #$00 ; RTS
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x01,        # $8000 LDX #$01    (Z=0)
            0xD0, 0x04,        # $8002 BNE $8008   (target = $8004 + 4)
            0xA9, 0xFF,        # $8004 LDA #$FF    (dead path)
            0xEA,              # $8006 NOP         (dead path)
            0x60,              # $8007 RTS         (dead path)
            0xEA,              # $8008 NOP         (live path)
            0x60,              # $8009 RTS
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)

    # Branch's surviving successor must be the taken target ($8008).
    assert _succ_pcs(graph, 0x8002) == [0x8008]

    # Dead-path insns ($8004 LDA, $8006 NOP, $8007 RTS) must have been
    # reachability-pruned; live-path insns ($8008 NOP, $8009 RTS) stay.
    pcs = _pcs(graph)
    assert 0x8000 in pcs and 0x8002 in pcs   # load + branch
    assert 0x8008 in pcs and 0x8009 in pcs   # live path
    assert 0x8004 not in pcs                 # dead LDA
    assert 0x8006 not in pcs                 # dead NOP
    assert 0x8007 not in pcs                 # dead RTS

    # Build report: one fold record.
    assert len(graph.const_z_folds) == 1
    f = graph.const_z_folds[0]
    assert f.branch_mnem == 'BNE'
    assert f.prev_mnem == 'LDX'
    assert f.prev_imm == 0x01
    assert f.z_value == 0
    assert f.taken_kind == 'jump'
    assert (f.live_pc24 & 0xFFFF) == 0x8008
    assert (f.dead_pc24 & 0xFFFF) == 0x8004


# ── Z=1 (LDX #$00) + BEQ → unconditional jump to taken target ───────

def test_ldx_imm_zero_then_beq_keeps_only_taken_edge():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x00,        # $8000 LDX #$00    (Z=1)
            0xF0, 0x04,        # $8002 BEQ $8008
            0xA9, 0xFF,        # $8004 LDA #$FF    (dead)
            0xEA,              # $8006 NOP         (dead)
            0x60,              # $8007 RTS         (dead)
            0xEA,              # $8008 NOP
            0x60,              # $8009 RTS
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    assert _succ_pcs(graph, 0x8002) == [0x8008]
    pcs = _pcs(graph)
    assert 0x8004 not in pcs and 0x8006 not in pcs and 0x8007 not in pcs
    assert 0x8008 in pcs and 0x8009 in pcs
    assert len(graph.const_z_folds) == 1
    assert graph.const_z_folds[0].branch_mnem == 'BEQ'
    assert graph.const_z_folds[0].z_value == 1


# ── Z=1 (LDX #$00) + BNE → unconditional fall (taken edge dead) ─────

def test_ldx_imm_zero_then_bne_keeps_only_fall_edge():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x00,        # $8000 LDX #$00    (Z=1)
            0xD0, 0x04,        # $8002 BNE $8008   (NOT taken)
            0xEA,              # $8004 NOP         (live fall)
            0x60,              # $8005 RTS         (live)
            0xA9, 0xFF,        # $8006 ...         (dead — only reached via taken edge)
            0xEA,              # $8008 NOP         (dead taken target)
            0x60,              # $8009 RTS         (dead)
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    assert _succ_pcs(graph, 0x8002) == [0x8004]
    pcs = _pcs(graph)
    assert 0x8004 in pcs and 0x8005 in pcs   # live fall
    assert 0x8008 not in pcs                 # dead taken target
    assert 0x8009 not in pcs
    assert graph.const_z_folds[0].taken_kind == 'fall'


# ── Z=0 (LDX #$01) + BEQ → unconditional fall (taken edge dead) ─────

def test_ldx_imm_nonzero_then_beq_keeps_only_fall_edge():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x01,        # $8000 LDX #$01    (Z=0)
            0xF0, 0x04,        # $8002 BEQ $8008   (NOT taken)
            0xEA,              # $8004 NOP         (live)
            0x60,              # $8005 RTS         (live)
            0xA9, 0xFF,        # $8006 dead
            0xEA,              # $8008 dead
            0x60,              # $8009 dead
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    assert _succ_pcs(graph, 0x8002) == [0x8004]
    pcs = _pcs(graph)
    assert 0x8008 not in pcs and 0x8009 not in pcs
    assert graph.const_z_folds[0].taken_kind == 'fall'


# ── Width: LDA #$00 in m=1 sets Z=1 ─────────────────────────────────

def test_lda_zero_m1_sets_z_for_beq():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA9, 0x00,        # $8000 LDA #$00 (m=1)  Z=1
            0xF0, 0x02,        # $8002 BEQ $8006
            0x60,              # $8004 RTS  (dead)
            0xEA,              # $8005 dead pad
            0x60,              # $8006 RTS  (live)
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    assert _succ_pcs(graph, 0x8002) == [0x8006]
    assert graph.const_z_folds[0].width_bits == 8
    assert graph.const_z_folds[0].z_value == 1


# ── Width: LDA #$0000 in m=0 sets Z=1 (16-bit imm) ──────────────────

def test_lda_zero_m0_sets_z_with_16bit_imm():
    # Entry m=0 — LDA #imm is 3 bytes.
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA9, 0x00, 0x00,  # $8000 LDA #$0000 (m=0)  Z=1
            0xF0, 0x02,        # $8003 BEQ $8007
            0x60,              # $8005 RTS (dead)
            0xEA,              # $8006 dead
            0x60,              # $8007 RTS (live)
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=0, entry_x=1)
    assert _succ_pcs(graph, 0x8003) == [0x8007]
    f = graph.const_z_folds[0]
    assert f.width_bits == 16
    assert f.z_value == 1


# ── Width: LDA #$0100 in m=0 sets Z=0 (high byte non-zero) ──────────

def test_lda_high_byte_only_m0_sets_z_zero():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA9, 0x00, 0x01,  # $8000 LDA #$0100 (m=0)  -> 16-bit imm = 0x0100, Z=0
            0xD0, 0x02,        # $8003 BNE $8007  (taken)
            0x60,              # $8005 RTS  (dead — only reached via fall)
            0xEA,              # $8006 dead
            0x60,              # $8007 RTS  (live)
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=0, entry_x=1)
    assert _succ_pcs(graph, 0x8003) == [0x8007]
    f = graph.const_z_folds[0]
    assert f.width_bits == 16
    assert f.prev_imm == 0x0100
    assert f.z_value == 0
    assert f.branch_mnem == 'BNE'
    assert f.taken_kind == 'jump'


# ── Width guard: a low byte that LOOKS zero but is part of a 16-bit
#                 imm whose high byte is nonzero must NOT misclassify as Z=1.
#
# This is the case `LDA #$0100` in m=0 covered above — included as a
# negative test against the temptation to mask only the low byte.

# ── Boundary guard: don't fold across a label / multi-predecessor branch.
#
# If two paths reach the BEQ with different prior insns, the static-Z
# proof from one path doesn't generalise. The fold's "single
# predecessor" guard prevents this.

def test_no_fold_when_branch_has_two_predecessors():
    # Two paths into $8004:
    #   path A (entry):    $8000 LDX #$01 ; $8002 BRA $8004
    #   path B:            $8004 BNE $800A   ← branch under test
    # And another path lands at $8004 via JMP from $8006:
    #   $8006 JMP $8004
    # The branch at $8004 has predecessors at $8002 (BRA) and $8006 (JMP)
    # — multiple. Fold must NOT apply.
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x01,        # $8000 LDX #$01
            0x80, 0x00,        # $8002 BRA $8004
            0xD0, 0x04,        # $8004 BNE $800A   (would fold if it had only one pred)
            0x4C, 0x04, 0x80,  # $8006 JMP $8004   (second pred for $8004)
            0x60,              # $8009 RTS (live fall fragment in case of no fold)
            0xEA,              # $800A NOP (taken)
            0x60,              # $800B RTS
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    # Branch at $8004 must keep both successors — no fold.
    succs = _succ_pcs(graph, 0x8004)
    assert sorted(succs) == [0x8006, 0x800A], (
        f"Expected branch at $8004 to keep both successors (no fold); "
        f"got {sorted(succs)}"
    )
    assert len(graph.const_z_folds) == 0


# ── Boundary guard: don't fold if predecessor's only successor isn't us.
#
# If the LDA/LDX/LDY's fall-through reaches us, but ALSO some side
# effect makes the load have more than one successor (shouldn't happen
# for IMM mode in practice), fold must not apply.
#
# Construction here is awkward because IMM-mode LDA has exactly one
# fall-through edge from _labeled_successors. The real-world way to
# violate the precondition is for an OTHER block to also reach the
# branch — covered by the two-predecessors test above. This test is
# kept symbolic.

# ── Boundary guard: don't fold across SEP/REP that touches Z indirectly.
#
# SEP/REP only touch P's m/x bits in the decoder model, NOT Z. But the
# narrow rule says the predecessor MUST be LDA/LDX/LDY immediate — so
# any non-load insn between the load and the branch already disqualifies
# the fold via the single-predecessor / single-successor checks.

def test_no_fold_when_intervening_instruction():
    # LDX #$01 ; NOP ; BNE — NOP between load and branch.
    # The branch's pred is NOT the LDX — it's the NOP. Even if the load
    # would set Z=0, the fold must not apply because the rule requires
    # the load to be the IMMEDIATE predecessor.
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x01,        # $8000 LDX #$01
            0xEA,              # $8002 NOP
            0xD0, 0x02,        # $8003 BNE $8007
            0x60,              # $8005 RTS
            0xEA,              # $8006 (filler)
            0x60,              # $8007 RTS
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    # No fold: branch keeps both successors.
    succs = _succ_pcs(graph, 0x8003)
    assert sorted(succs) == [0x8005, 0x8007]
    assert len(graph.const_z_folds) == 0


# ── Smoke: insn flag is set when fold applies, cleared when it doesn't.

def test_insn_flag_set_only_on_folded_branch():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x01,        # $8000 LDX #$01
            0xD0, 0x00,        # $8002 BNE $8004   (will fold — Z=0, taken == fall here)
            0x60,              # $8004 RTS
        ]),
    })
    graph = decode_function(rom, 0, 0x8000, entry_m=1, entry_x=1)
    # Whether the fold lands on jump or fall depends on offset; for
    # offset=0 the BNE target == fall-through. Either way the flag
    # is set on the branch insn.
    bne = _insn_at(graph, 0x8002).insn
    assert bne.const_z_fold_unconditional is True
    # Predecessor LDX is unaffected.
    ldx = _insn_at(graph, 0x8000).insn
    assert ldx.const_z_fold_unconditional is False
