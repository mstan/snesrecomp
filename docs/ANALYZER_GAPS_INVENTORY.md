# Analyzer Gap Inventory — 65816 static recompilation

**Audience:** anyone porting snesrecomp to a new ROM, or adding a new
consumer of decoder/CFG/codegen output. Read before assuming a static
property holds.

**Status:** living document. Each new gap discovered in the wild —
whether closed in the same session or deferred — adds an entry here
**before** the fix lands, so the inventory grows monotonically with
the analyzer's coverage.

## Why this document exists

A static recompiler is a pile of small abstract-interpretation
analyses (M/X-flag tracking, stack-depth tracking, direct-page
tracking, callee-effect propagation, indirect-call resolution) plus a
codegen that consumes them. Every analyzer is, by construction,
**incomplete**: there exist runtime constructs it does not model.
Each unmodeled construct is a **gap**. Hit a gap and the analyzer's
output is silently wrong; the symptom shows up days later as a
visual glitch, a freeze, or a "Mario dies on slope."

Through 9 months of SMW recompilation work we have closed ~40 such
gaps. None of them was anticipated in advance — each one was
discovered by hitting it. The "whack-a-mole" pattern is what this
document exists to break.

The bet: **the 65816's gap surface is finite and bounded**. 256
opcodes, a fixed set of stack tricks, a fixed set of dispatch
idioms, and a fixed set of hardware quirks. We can enumerate the
surface from primary sources and our own bug history, mark each
entry CLOSED / OPEN / N/A-for-SMW, and from then on the cost of the
next port (or the next consumer) is bounded by the surface, not by
how many sites happen to trip.

## Relationship to other docs

- [`ABSTRACT_INTERPRETATION_GAPS.md`](./ABSTRACT_INTERPRETATION_GAPS.md)
  is the **M/X-flag spine** of this inventory — category A below
  delegates detail to it. This document is the superset.
- [`TRIPWIRES.md`](./TRIPWIRES.md) catalogues the runtime tripwires
  that detect unmodeled constructs. Many entries below cross-
  reference a tripwire.
- `TROUBLESHOOTING.md` is the operational workflow for when a
  tripwire fires; this document is the structural reference for
  what each tripwire is watching.

## Sources synthesized

- WDC 65C816 datasheet (opcode table, addressing-mode reference,
  flag-effect column)
- Eyes & Lichty, *Programming the 65816*, especially ch. 18
  ("Programming the W65C816") on stack-tricks and ABI conventions
- Martin Korth, fullsnes.txt § "CPU 65816" (decimal mode quirks,
  e-flag transitions, BRK/COP vector behavior, interrupt entry
  side-effects)
- snes9x `cpu.cpp` / `cpuops.cpp` / `cpuexec.cpp` — every idiom
  emulator handles specially is a candidate gap for a static
  recompiler
- bsnes-plus `cpu/*` — higher-accuracy emulator; same use
- N64Recomp (Wiseguy) analyzer.md — MIPS-side, but the
  abstract-interpretation patterns transfer
- snesrecomp commit history (140+ commits 2026-04-19 → 2026-05-17),
  especially the `v2 phase 6*` series and the `feat/nlr-return-abi`
  branch
- snesrecomp memory entries (27 entries cataloguing closed root
  causes)
- SMW visible-bug reports — each one mapped back to its analyzer
  gap class

## How to use

When adding a consumer of analyzer output (e.g., a new codegen
emitter, a new auto-router, a new optimization pass):

1. List the analyzer properties your consumer assumes (e.g., "I
   assume `insn.m_flag` is sound", "I assume `S` is balanced from
   function entry").
2. For each assumption, find the relevant gap category below.
3. Confirm every entry in that category is either CLOSED for SMW,
   or your consumer handles the OPEN entry explicitly.
4. If an entry is OPEN and your consumer doesn't handle it, either
   close the gap first, or document the explicit blast radius in
   your consumer's docstring.

When hitting a runtime symptom you can't explain:

1. Identify the analyzer property the symptom indicts (wrong width
   → M/X gap; -N stack drift → stack-discipline gap; "calls into
   wrong bank" → DB or function-finder gap).
2. Walk the relevant category looking for an OPEN entry whose
   detection signature matches.
3. If no entry matches, the symptom is either a new gap (add an
   entry below) or a non-analyzer bug (codegen mistake at a
   correctly-analyzed site).

When porting to a new ROM:

1. Walk every CLOSED-for-SMW entry. Re-verify the assumption holds
   for the new ROM — many entries are "true for SMW because SMW
   doesn't use construct X; not true in general."
2. Walk every N/A entry — same.
3. Walk every OPEN entry — your ROM may hit one we never did.

## Notation

- **CLOSED**: the analyzer correctly models the construct, verified
  by tests + runtime tripwires.
- **OPEN**: the analyzer does not model the construct; runtime
  symptoms reach user-visible behavior or are masked only by luck.
- **N/A-SMW**: SMW does not use the construct; analyzer is unsound
  if exposed to it. Treat as OPEN when porting.
- **PARTIAL**: handled for one shape (e.g., one trampoline pattern)
  but not the general case. Cite the shape that's handled.

Each entry carries: construct, runtime behavior, analyzer failure
mode, detection signature, status, reference (commit / doc).

---

## Category A — M/X-flag transfer functions

Authoritative detail lives in
[`ABSTRACT_INTERPRETATION_GAPS.md`](./ABSTRACT_INTERPRETATION_GAPS.md).
Index only here.

### A1. REP / SEP immediate-mask
- **Construct:** `REP #imm`, `SEP #imm` clear / set M/X bits per
  immediate mask.
- **Status:** CLOSED. Modeled in `decoder.post_state`.

### A2. PHP / PLP bracketed flag save-restore
- **Construct:** `PHP` pushes P, `PLP` restores it. M/X live inside P.
- **Status:** CLOSED 2026-05-15. `p_stack` of depth ≤8 added to
  `DecodeKey`. Snesrecomp `73e3d26`.
- **Detection:** UpdateSaveBuffer -2 stack drift on overworld save
  path was the canonical user-visible symptom.

### A3. Non-leaf callee-exit-MX inference
- **Construct:** caller's M/X across `JSR`/`JSL` when callee internally
  changes M/X.
- **Status:** CLOSED 2026-05-16. Per-variant fixpoint over the call
  graph. Snesrecomp `43266e2` + `808e918`.
- **Detection:** "Mario dies on slope" (RunPlayerBlockCode -1 drift)
  was the canonical symptom. `mx_claim_check` runtime tripwire arms
  at boot and catches residuals.

### A4. RTI mid-function
- **Construct:** `RTI` pops P (and PC) from stack. If RTI appears
  mid-body (not as a handler terminator) and decoded successors
  follow, those successors need to reflect the popped-P state.
- **Status:** N/A-SMW. SMW handlers RTI only at their tail.
- **Detection:** would manifest as wrong widths on instructions
  after a mid-body RTI in an interrupt handler.

### A5. XCE (Exchange Carry/Emulation) repeated mid-execution
- **Construct:** `XCE` swaps C↔E. If E becomes 1, M and X are
  forced to 1 and D forced to 0.
- **Status:** N/A-SMW. XCE executes once at I_RESET, never again.
- **Detection:** any ROM that re-enters emulation mode in normal
  flow would expose this.

### A6. Unbalanced PHP/PLP across function boundaries
- **Construct:** callee pushes P without matching pop, or pops
  without matching push.
- **Status:** N/A-SMW. SMW callees are well-balanced.
- **Detection:** `p_stack` underflow / non-empty at function exit;
  symptom would be flag-state leaking across calls.

### A7. Async M/X writes from NMI/IRQ context
- **Construct:** runtime NMI/IRQ handler entry/exit pushes and
  restores P including M/X. The static decoder, looking only at
  the current function's bytes, cannot see these.
- **Status:** OPEN-latent. DA49 verifier trip 2026-05-16 (benign at
  site because DA49 opens with REP #$30). Runtime tripwire
  `mx_async_check` arms at boot.
- **Detection:** `mx_async_check_get` returns a snapshot if
  m_flag / x_flag changes without a corresponding
  `g_px_mutation_count` increment between consecutive block hooks.

### A8. PHP/PLP-balanced *whole-function* classification
- **Construct:** a function bookends with `PHP` … `PLP`; its
  internal SEP/REP do not leak. From a caller's POV, the
  function's exit M/X equals its entry M/X.
- **Status:** OPEN. p_stack tracks the push/pop semantics but the
  auto-router does not classify a function as "PHP/PLP-balanced
  flag preserver" and skip exit-MX inference for it. Manifests in
  ALttP `bank_00_8888_M1X1` (transitively reached from I_RESET via
  JSR $8901): mid-body M1X0 decode is correct under p_stack but
  the function's *exit* M/X claim drifts.
- **Detection:** decoder reports an exit-MX that differs from
  entry-MX for a function whose body has a top-level
  PHP/…/PLP bracketing.

---

## Category B — Other P-flag tracking (D, C, V, N, Z, I)

### B1. Decimal mode (D flag) tracking
- **Construct:** `SED` sets D, `CLD` clears it. ADC/SBC behave
  decimally when D=1.
- **Status:** N/A-SMW. SMW does not use decimal mode in normal flow.
- **Detection:** any post-SED ADC/SBC that emits as binary would
  silently produce wrong results.

### B2. Carry-in liveness across function calls
- **Construct:** caller's C flag consumed by callee's ADC/SBC
  before any C-writer runs in callee.
- **Status:** CLOSED. ADC-#0 carry-fold result is now uint8/uint16-
  truncated (snesrecomp `065b66f`); `kPatchedCarrys_SMW` was
  retired 2026-04-20 because the patched sites were dead.
- **Detection:** the `FixupCarry()` BRK loop in the old interpreter
  was the historical detector.

### B3. Constant-Z fold from immediate load + branch
- **Construct:** `LDA #imm` … `BEQ`/`BNE` where the decoder can
  decide the branch direction at compile time.
- **Status:** CLOSED. Narrow fold in v2 decoder, snesrecomp
  `c3ea87e`. Deliberately narrow to avoid bloating the abstract
  domain.

### B4. N/Z flag-setting on indirect address compute
- **Construct:** ops like `LDA [dp]` set N/Z based on the loaded
  value's high bit / zero-ness; codegen must compute the value
  before the flag set.
- **Status:** CLOSED. INC/DEC mem uses IncMem (no carry-in)
  + propagates flags (snesrecomp `300cd93`). LDA/LDX/LDY set N/Z
  on load (`5d7cad9`).

### B5. V (overflow) flag set/clear semantics
- **Construct:** `CLV`, `SEC`/`CLC` interaction with BIT, ADC, SBC.
- **Status:** CLOSED. V is computed by ADC/SBC lowering; CLV clears
  it; BIT writes V from bit 6.

### B6. I (interrupt-disable) flag for NMI/IRQ gating
- **Construct:** `SEI` / `CLI` gate IRQ entry. NMI is non-maskable.
- **Status:** N/A for static analyzer (it's a runtime thing). The
  runtime honors I correctly via `cpu.c`.

---

## Category C — Stack discipline / S pointer drift

This category caused the largest number of user-visible bugs in
SMW. Stack drift cascades: a -1 / +1 / -2 in one function corrupts
the return PC of its caller, which mis-decodes when control returns,
which may corrupt further state, etc.

### C1. Push/pull width pinning
- **Construct:** `PHA`/`PHX`/`PHY` push 1 or 2 bytes depending on
  current M (for A) or X (for X/Y). Matching `PLA`/`PLX`/`PLY`
  must consume the same width.
- **Status:** CLOSED. Snesrecomp `1e476e6` (PHA/PHX/PHY) + later
  reapplies. Width pinned to decoder's static `insn.m_flag` /
  `insn.x_flag`.
- **Sensitivity:** consumes category A's M/X soundness. Safe on
  REP/SEP-only and PHP/PLP-bracketed paths.

### C2. Non-local return via PLA/PLA/RTS (skip-grandparent idiom)
- **Construct:** function decides not to return to its caller;
  instead `PLA / PLA / RTS` pops the caller's return PC off the
  stack and returns to the grandparent. SMW uses this in
  `GetDrawInfo_Bank01_Recomp_M1X1` BNE→$01:A3CB.
- **Status:** CLOSED. RECOMP_RETURN_SKIP_N ABI; function-local
  `_pending_skip`; NLR pattern detect in v2 (snesrecomp `bf2c147`,
  `b9e0289`, `f9f48a5`).
- **Detection:** dynamic stack-drift tripwire fires at function
  exit with S_delta = ±N for the unmodeled cases.

### C3. Dispatch trampoline (PHK + PER + JML pattern)
- **Construct:** asm sequence `PHK; PER <displ>; JML <dispatcher>`
  where PHK + PER push dispatch ARGS for the dispatcher to consume.
  The bytes are not part of a calling convention — they're inline
  data. Stack appears unbalanced if read as standard pushes.
- **Status:** CLOSED. v2 codegen auto-skips PHK/PEA/PER preceding
  a JML-with-dispatch_entries and routes the JML through
  `_emit_dispatch` (snesrecomp `e6cc093`).
- **Detection:** stack-drift tripwire fires with -3 delta on the
  trampoline.

### C4. RTS Trick (push computed address + RTS to it)
- **Construct:** caller computes a target PC, pushes
  `target_pc - 1` (in two PHA's), then RTS. RTS reads it back and
  jumps. Classic 65xx jump-table technique.
- **Status:** N/A-SMW. SMW uses dispatch trampolines (C3) instead.
- **Detection:** decoder would see PHA / PHA / RTS with no
  recognized pattern; tripwire would fire on the synthesized
  return.

### C5. JSL/RTL pseudo-NLR
- **Construct:** like C2 but with the 4-byte long-call frame
  (RTL pops 3 bytes vs RTS's 2).
- **Status:** PARTIAL. NLR detector handles 2-byte (RTS) and
  3-byte (RTL) cases. Mixed RTS/RTL is undefined behavior.

### C6. Tail-call fallthrough
- **Construct:** function A's last instruction falls into function
  B's entry (no JSR/JMP). Caller of A expects to return; in fact
  control flows into B and B's RTS returns to A's caller.
- **Status:** CLOSED. v2 auto-detects (snesrecomp `7203539`); cfg
  `tail_call:<addr>` directive for explicit two-entry-points-
  sharing-body (`f4c764d`).

### C7. Fall-through into excluded range
- **Construct:** function body's natural fall-through PC lands in
  a cfg `exclude_range`. Old recomp emitted a tail call to the
  excluded function (which doesn't exist), leaving the stack
  imbalanced.
- **Status:** CLOSED. `_emit_function` now emits `RecompStackPop
  + return` on non-terminal bodies with no valid fall-through
  target (historical autonomous-rip session).

### C8. Stack-pointer underflow at function entry
- **Construct:** function entered with S already in the
  hardware-stack-range guard ($01F8 or below for SMW); a single
  push would underflow into the hardware vector area.
- **Status:** CLOSED (detection). Runtime tripwire on S out of
  $01XX-$1FFF arms by default. Static side does not enforce.

### C9. Cross-bank goto to non-entry label of another function
- **Construct:** v2 emits a synthesized BRA into the middle of a
  different (already-emitted) function's body via a label. Cross-
  function-goto.
- **Status:** PARTIAL. 42 instances tagged 2026-05-02 with stub-
  return; some closed by cross-fn-block import (`e163931`),
  remainder trapped explicitly (`cd4c463`) instead of silent-return.
- **Detection:** phantom-PC trap covers unresolvable-goto source
  PCs (`2c57271`).

---

## Category D — Direct page (D) register tracking

The D register is the base for direct-page addressing modes
(`LDA dp`, `LDA (dp),Y`, etc.). When D ≠ 0, dp addresses map
elsewhere in bank 0.

### D1. D=0 default assumption
- **Construct:** decoder assumes D=0 for all direct-page address
  computations.
- **Status:** PARTIAL-OPEN. SMW mostly uses D=0. A few banks
  (notably bank 02/03 sprite code in some idioms) deliberately
  remap D temporarily.
- **Detection:** would manifest as direct-page reads/writes hitting
  the wrong WRAM slot. No tripwire today.

### D2. PHD / PLD tracking
- **Construct:** `PHD` pushes D (2 bytes); `PLD` pops.
- **Status:** OPEN. Decoder does not model D in its abstract state.
  Push/pull width is correct (always 16); the effect on subsequent
  dp-addressing is invisible.
- **Detection:** synthesizable as a static tripwire ("after PLD,
  any subsequent dp-mode insn is suspect").

### D3. TCD / TDC
- **Construct:** transfer C↔D. `TCD` makes A the new D.
- **Status:** OPEN. Same as D2.

---

## Category E — Data bank (DB) register tracking

DB sources the high byte for absolute (`LDA $XXXX`) and
absolute,X/Y addressing. Wrong DB at runtime means cross-bank
reads hit the wrong bank.

### E1. PHB / PLB across JSL wrappers
- **Construct:** `PHB; PHK; PLB; JSR body; PLB; RTL` wrapper saves
  caller's DB, sets DB=PB for the body's duration, restores.
  Cross-bank `JSL` to the body (instead of the wrapper) bypasses
  this and runs body with caller's DB.
- **Status:** CLOSED for SMW's 5 known instances ($01:802A, $01:8042,
  $01:90B2, $01:9138, $02:B81C). Wrapper-bypass detector + cfg
  template (commit `9dc3131`). v2 auto-routes via `wrapper_autoroute`
  (`c457ea0`).
- **Detection:** ring-verified `DB=$01` at body entry post-fix
  (was `$02`/`$03`).

### E2. PLB target inference
- **Construct:** generic `PLB` (not in the wrapper idiom) changes
  DB. Subsequent absolute reads use the new DB.
- **Status:** OPEN. Decoder does not track DB.
- **Detection:** would require a tripwire on per-instruction DB
  matching cfg-expected DB for the function.

### E3. JSL bank propagation
- **Construct:** `JSL targetbank:addr16` pushes PB, jumps to target;
  the callee's DB at entry is the *caller's* DB unless the callee
  changes it. Code at the target's entry that does `LDA $XXXX`
  reads from caller's DB.
- **Status:** PARTIAL. SMW callees that need their own DB use the
  E1 wrapper. Unwrapped JSL targets are assumed to either be
  DB-agnostic or to set their own DB.
- **Detection:** historical bug class — see PIRANHA fix (`9dc3131`
  template).

### E4. Indexed read with carry into bank ($BB93AB class)
- **Construct:** `LDA $XXXX,X` with wide X (x=0) where `$XXXX + X
  >= $10000`. The 65816 may wrap modulo bank or escalate
  depending on addressing mode and m/x flags.
- **Status:** OPEN. Detected by off-rails RomPtr tripwire 2026-05-16
  in `BufferScrollingTiles_Layer1_VerticalLevel_M1X1`. Mirrored to
  safe location at runtime; user did not observe gameplay impact.
- **Detection:** `offrails_get` TCP cmd; one bucket per `(tag,
  high-16-of-hint)` pair. `[RomPtr-invalid]` tag with hint outside
  $00-$0F bank range.

---

## Category F — Control flow modeling

### F1. JMP (abs) indirect through ROM table
- **Construct:** `JMP ($XXXX)` reads the target from `$DB:XXXX`
  (well, with bank quirks).
- **Status:** PARTIAL. SMW's ExecutePtr / ExecutePtrLong
  trampolines are detected by `classify_dispatch_helper` and
  rewritten as synthesized C `switch` + direct calls. Generic
  `JMP (abs)` outside this pattern → cfg-required-dispatch-or-kill
  (`162511a`).

### F2. JMP (abs,X) jump-table dispatch
- **Construct:** `JMP ($XXXX,X)` reads target from `$DB:XXXX + X*2`.
- **Status:** CLOSED with mandatory cfg `dispatch_entries`
  declaration. v2 rejects if no cfg entry (`162511a`). Validity
  gate rejects all-FF / all-00 targets (`27c4aee`).

### F3. JML [abs] long-indirect dispatch
- **Construct:** `JML [$XXXX]` reads 24-bit target.
- **Status:** PARTIAL. Same handling as F1.

### F4. JSR (abs,X)
- **Construct:** indirect call via X-indexed table.
- **Status:** CLOSED with cfg `indirect_call_table` requirement
  (`162511a`).

### F5. Self-modifying code (SMC)
- **Construct:** code that writes to its own ROM/RAM image and
  then executes the modified bytes.
- **Status:** PARTIAL. v2 decoder REJECTS WRAM/SRAM-resident code
  addresses. SMW's one SMC routine (DecompressTo inner loop) is
  HLE'd in `src/gen_stubs.c`. Phantom-PC trap covers
  CALL_INDIRECT sites empirically proven dead (`407a511`).
- **Detection:** PC entering WRAM range → phantom-PC trap fires.

### F6. WRAM-resident code execution
- **Construct:** code copied to WRAM (e.g., $7E:2000+) and JSR'd.
- **Status:** N/A-SMW (only the DecompressTo case, HLE'd). General
  rejection by the decoder; would need cfg-declared WRAM-resident
  function template otherwise.

### F7. Branch-into-instruction (phantom decode)
- **Construct:** a BCC/BCS lands on an operand byte of a preceding
  instruction (e.g., the `#$8510` operand of a mis-decoded ADC).
- **Status:** CLOSED via category A3 — mode-state recovery prevents
  most. Phantom-PC trap (`407a511`) catches residuals at runtime.
- **Detection:** sentinel-decode at the phantom PC; trap fires
  with bucket containing decoded-as opcode.

### F8. Computed PB via PHK / PLB-style trick targeting code
- **Construct:** push computed bank byte, set PB indirectly. SMW
  doesn't do this except in dispatch trampolines (C3).
- **Status:** N/A-SMW.

### F9. End-of-bank wrap on PC
- **Construct:** PC increments past $FFFF in bank N; behavior is
  to wrap to $0000 in same bank, not advance to bank N+1.
- **Status:** N/A-SMW. SMW functions don't span the end of bank.
- **Detection:** would manifest as wrong decode for an instruction
  spanning the boundary.

### F10. BRK / COP vectors mid-function
- **Construct:** `BRK` / `COP` push PB+PC+P, jump to vector. If
  decoded successors follow, they need to model the vector entry.
- **Status:** CLOSED (terminator). v2 treats BRK/COP as no-successor
  terminators. SMW doesn't BRK in normal flow.

### F11. WAI / STP (CPU halt)
- **Construct:** halt-until-interrupt / hard stop.
- **Status:** CLOSED (terminator).

---

## Category G — Codegen width/zero-extend correctness

These are not *analyzer* gaps strictly — they're codegen bugs at a
correctly-analyzed site — but they belong here because the analyzer
is the source of truth the codegen consumes, and these closed-class
shapes recur enough to warrant cataloguing.

### G1. ASL / LSR / ROL / ROR width-mask
- **Construct:** in M=1, ASL A must mask out A's high byte (B);
  LSR result must zero high byte.
- **Status:** CLOSED. Snesrecomp `8f9369d`. **Closed palette
  corruption + collision class.**
- **Detection:** unit + multi-insn fuzz `5262388`; lint
  `c817650`.

### G2. ADC / SBC / CMP operand-mask
- **Construct:** in M=1, both operands must be masked before ALU
  + carry/overflow compute.
- **Status:** CLOSED. Snesrecomp `5c00d95`. **Closed HexToDec hang.**

### G3. 8-bit X / Y zero-extend on writes
- **Construct:** SEP #$10 + LDY $XX (8-bit) must zero-extend
  cpu->Y to 16 bits; hardware contract is "zeroes are gospel"
  for the high byte.
- **Status:** CLOSED. Snesrecomp `b39e99b`. **Closed LoadStripeImage
  hang.**

### G4. XBA stale-shadow
- **Construct:** XBA swaps A.low ↔ A.high. Prior implementation
  had a separate `cpu->B` field that drifted.
- **Status:** CLOSED. Snesrecomp `6c04c94` + `84b359e` (deleted the
  field entirely). **Closed Layer-3 / HUD attract scramble.**

### G5. SEP / REP P-mirror clobber
- **Construct:** SEP/REP codegen called `cpu_p_to_mirrors` with
  stale `cpu->P`, clobbering freshly-set `_flag_Z` from intervening
  DEC/INC/ALU ops.
- **Status:** CLOSED. Snesrecomp `44c96a7`. **Closed GameMode $FF
  corruption.**

### G6. STA [dp] / STA [dp],Y in M=0 high-byte drop
- **Construct:** wide-store through long-indirect dp dropped the
  high byte silently (fell through to a no-op comment).
- **Status:** CLOSED. New `IndirWriteWord` runtime inline;
  `_emit_sta16` covers INDIR_Y / INDIR_DPX / DP_INDIR.

### G7. MVN / MVP src/dst bank swap
- **Construct:** block-move src/dst banks encoded in
  reverse-of-asm-syntax in the opcode bytes.
- **Status:** CLOSED. Snesrecomp `e5e369a`.

### G8. Width-mask DRY refactor
- **Construct:** 11 ad-hoc width-derivation sites scattered across
  emit code; 57 raw width literals. A chokepoint refactor was
  needed to stop reactive-patching width bugs at each site.
- **Status:** CLOSED. Snesrecomp `fa09fef` (widths.py) + `c817650`
  (lint + shape tests) + `63546ac` (multi-insn fuzz). Per memory:
  "DRY when the same shape of bug fixes twice."

### G9. Indexed WRAM effective-address wrap at 16 bits
- **Construct:** in m=0/x=0, `LDA $XXXX,X` with X large; the
  effective WRAM address must wrap modulo $10000 (not modulo
  $20000 of the address space).
- **Status:** CLOSED. Snesrecomp `64544e8`.

### G10. ADC carry-fold result truncation
- **Construct:** `ADC #0` decoded as a no-op breaks the carry
  chain; the fold must still propagate C and truncate result to
  op width.
- **Status:** CLOSED. Snesrecomp `065b66f`.

### G11. INC/DEC mem flag setting via dedicated IR op
- **Construct:** previous lowering routed INC/DEC mem through
  `Alu(ADD/SUB)`, which spuriously consumed C and wrote V.
- **Status:** CLOSED. New `IncMem` IR op (snesrecomp `300cd93`).

### G12. Transfer / PullReg flag + width
- **Construct:** `TAX`, `TAY`, etc. + `PLA`, `PLX`, `PLY` must
  set N/Z on result and respect current width.
- **Status:** CLOSED. Snesrecomp `05ca8aa`.

### G13. LDA/LDX/LDY N/Z flag setting on load
- **Construct:** loads must set N/Z based on the loaded value.
- **Status:** CLOSED. Snesrecomp `5d7cad9`.

### G14. PHA/PHX/PHY/PLA/PLX/PLY width pinning to static (m,x)
- **Construct:** width inferred at decode time, pinned in IR;
  prevented PLP-restored mismatches.
- **Status:** CLOSED. Snesrecomp `1e476e6` + reapplies. Sensitivity
  to category A's M/X soundness (now closed).

---

## Category H — Function-finder / CFG-builder correctness

### H1. Cross-bank `name <pc> <body_name>` aliasing the wrong PC
- **Construct:** cfg `name 02b81c X` aliased a wrapper PC to its
  body's name, so cross-bank `JSL` resolved past the wrapper.
- **Status:** CLOSED. Wrapper-bypass class. SMW commit `9dc3131`
  template (and `c457ea0` in snesrecomp). Pokey segment-eat fix is
  the canonical example.

### H2. Function-finder conflating same-name callsites
- **Construct:** two functions named `GenericSprGfxRt0` and
  `GenericSprGfxRt2` at $01:8042 / $01:9F0D respectively;
  recompiler routed all bank-02 JSL callers to the wrong one.
- **Status:** CLOSED (Class 1 dispatch bug). Issue G Piranha plant
  fix.

### H3. Dispatch helper ordering
- **Construct:** `auto_detect_dispatch_helpers` must run before
  `discover_bank` so promotions don't mask real handlers.
- **Status:** CLOSED. Snesrecomp `8f0893d`.

### H4. Dispatch-table truncation by all-FF/00 padding
- **Construct:** auto-detected dispatch readers read past the
  table's real end into ROM padding.
- **Status:** CLOSED. Snesrecomp `27c4aee` + `83464f3`.

### H5. Dispatch-extent multipass (cross-bank thunk-sig)
- **Construct:** dispatch-table extent + the cross-bank thunk
  signature must agree; a single pass mis-orders.
- **Status:** CLOSED. Branch `dispatch-extent-multipass` merged.

### H6. JSR/JSL targets that aren't valid LoROM code addresses
- **Construct:** decoder follows a bogus call target into MMIO or
  unmapped ROM.
- **Status:** CLOSED. Snesrecomp `7394869` rejects.

### H7. Sig narrowing (one-way widen, never narrow)
- **Construct:** `_augment_sig_with_livein` deliberately widens
  but never narrows. Once a function gets `(uint8 k)` introduced,
  it's pinned forever even if `k` is never read.
- **Status:** OPEN. ISSUES.md framework gap (autonomous-rip
  session 2026-04-20). Cosmetic in current SMW (the `k` parameter
  is dead in every dispatched site) but the next dispatch target
  that actually reads X would silently miscompile.

### H8. Cross-BB X tracking in caller (loop back-edges)
- **Construct:** `_build_call_args`'s in-BB X tracker is reset at
  each block; a call after a loop back-edge sees `self.X = None`,
  emits `0` with WARN.
- **Status:** OPEN. Same source as H7. Workaround: per-site
  audit.

### H9. Cross-function-block import (DB=$C0 root)
- **Construct:** v2's earlier inability to inline blocks across
  function boundaries caused the DB=$C0 dispatch entry corruption
  class.
- **Status:** CLOSED. Snesrecomp `e163931` + `bb67ce6` (stack-
  drift tripwire + boundary exit-kind tagging).

### H10. Dispatch-extent over-promotion of phantom entries
- **Construct:** auto-promote inside MANUAL func body promotes
  phantom entries (e.g., `auto_01_ECEC` caps `Spr035_Yoshi`'s
  emit range). Three different heuristics tried 2026-04-26 all
  regressed. Test-locked.
- **Status:** OPEN-blocked. See ISSUES.md Issue C. Requires a
  more discriminating heuristic; regression-locked by
  `test_attract_demo_regression`.

---

## Category I — Cross-bank ABI / wrapper handling

Closely related to E (DB tracking) and H (function-finder), but
called out separately because the wrapper-class bug surfaced in
3 distinct SMW visible bugs.

### I1. PHB/PHK/PLB/JSR/PLB/RTL wrapper template
- See E1, H1. CLOSED.

### I2. Wrapper signature lint
- **Construct:** a future-proof lint to detect
  `name <pc> <fn>` directives where the bytes at `<pc>` match
  `8B 4B AB 20 LO HI AB 6B` (the wrapper signature) and `<fn>`'s
  declared PC ≠ `<HI:LO>`.
- **Status:** OPEN-queued. Would catch all 5 known wrappers
  automatically. ISSUES.md framework-cleanup queue.

### I3. Latent wrapper-bypass candidates
- **Construct:** $01:801A, $01:8022, $01:8032, $01:803A — same
  wrapper pattern, but their bodies access only mirrored low-RAM
  (<$2000) so DB-mismatch is invisible.
- **Status:** OPEN-latent. Tracked in ISSUES.md "may be silent."

### I4. Dispatch synthesis SEP #$30 mirror
- **Construct:** the synthesized C `switch` for an ExecutePtr-style
  dispatch must mirror the trampoline's SEP #$30 (force m=x=1) at
  the synthesized switch entry.
- **Status:** CLOSED. Snesrecomp `7da1cfe` + reapply `cf36453`.

---

## Category J — Hardware quirks

### J1. NMI / IRQ entry forces native mode
- **Construct:** hardware NMI/IRQ entry pushes P+PC, sets I=1,
  forces native-mode (m,x) at vector.
- **Status:** CLOSED. Handlers decoded as separate cfg entries
  with declared `entry_m, entry_x`. RTI restores from hardware-
  pushed P.

### J2. e-flag (emulation mode) re-entry
- **Construct:** XCE with C=1 enters emulation mode; m,x forced
  to 1; D forced to 0.
- **Status:** CLOSED for boot-only (SMW does XCE once at I_RESET).
  N/A-SMW for repeated XCE.

### J3. NMI race on frame 0
- **Construct:** at boot, the runtime may schedule an NMI before
  I_RESET completes, corrupting frame-0 state.
- **Status:** CLOSED. Snesrecomp `3b6fc37` skips I_NMI on frame 0.

### J4. DMA channel programming
- **Construct:** HDMA / DMA channel registers $4300-$437F must be
  programmed before $420C / $420B trigger.
- **Status:** PARTIAL. Runtime models DMA. Static analyzer treats
  $43xx writes as opaque MMIO.

### J5. PPU register write ordering
- **Construct:** specific PPU registers ($2100-$213F) have
  write-twice ($2116/$2117 for VRAM addr, etc.) and
  open-bus quirks.
- **Status:** PARTIAL. Runtime PPU models it. Static analyzer
  treats $21xx as opaque MMIO.

### J6. APU timing pace
- **Construct:** APU catchup must be paced by elapsed main-CPU
  cycles, not by hardcoded frame ticks.
- **Status:** CLOSED. Snesrecomp `5c55cea`.

### J7. Virtual hardware timing
- **Construct:** $4210 read-clear, $4212 h-counter, $4216 RMW
  dispatch.
- **Status:** CLOSED. Snesrecomp `2f22347`.

### J8. Joypad register routing
- **Construct:** $4016/$4017 manual joypad reads must go through
  `ReadReg` (not direct memory).
- **Status:** CLOSED. Snesrecomp `2c3eb99`.

### J9. snes9x_bridge frame timing
- **Construct:** bridge's `run_frame` must yield on NMI, not on
  next vblank.
- **Status:** CLOSED. Snesrecomp `136b6fa`.

### J10. Decimal mode quirks (ADC/SBC with D=1)
- **Construct:** in decimal mode, ADC/SBC produce BCD results.
  Different carry/V semantics.
- **Status:** N/A-SMW (D never set). See B1.

---

## Category K — Memory bus routing

### K1. ROM read out-of-range
- **Construct:** `cart_readLorom` for an address past `rom_size`.
- **Status:** CLOSED. Soft-return 0 (snesrecomp `2cf6bc6`).

### K2. RomPtr clamping
- **Construct:** `RomPtr` for an out-of-range address; previously
  SIGSEGV'd.
- **Status:** CLOSED. Snesrecomp `9c6ac4b` clamps to rom_size.

### K3. SRAM bank routing for LoROM / HiROM
- **Construct:** `cpu_read*` / `cpu_write*` must route SRAM bank
  windows correctly per mapper.
- **Status:** CLOSED. Snesrecomp `ba97eb7`.

### K4. WRAM mirroring banks $00-$3F and $80-$BF
- **Construct:** $0000-$1FFF in low banks mirrors WRAM
  $7E:0000-$7E:1FFF.
- **Status:** CLOSED (runtime handled).

### K5. Open bus reads
- **Construct:** reads from unmapped MMIO return last bus value.
- **Status:** PARTIAL. Most SMW reads avoid open bus; off-rails
  tripwire catches residuals.

---

## How to add a new gap entry

When a new gap is discovered:

1. Classify it into A–K (or open category L for a new dimension).
2. Write the entry in the same format: construct, runtime
   behavior, analyzer failure mode, detection signature, status,
   reference.
3. Mark status OPEN if the fix isn't in yet; flip to CLOSED with
   the commit ref when it lands.
4. Add a runtime tripwire if one doesn't already cover this
   detection signature.
5. Update the SMW memory log if it's a notable closure.

## Verification checklist when shipping a new analyzer consumer

Before any consumer of decoder/CFG/codegen output lands:

- [ ] Listed every analyzer property the consumer assumes.
- [ ] For each property, confirmed it's CLOSED in this inventory
      (or the consumer handles the OPEN entry explicitly).
- [ ] Added a unit test that exercises the property through each
      relevant M/X variant.
- [ ] Ran the runtime static-claim verifier
      (`mx_claim_check_get`) against a known-playable region; no
      new trips.
- [ ] Ran `mx_async_check_get`; no new trips.
- [ ] Ran the always-on stack-drift tripwire across a 5-min
      playthrough; no new trips.
- [ ] If the consumer emits new opcodes / IR shapes, ran the
      multi-insn fuzz harness.

## Open questions

- **Does p_stack depth 8 suffice?** Defensive choice. SMW typical
  is 0–1. Worth measuring at regen and flagging if any function
  needs more.
- **Should DB be added to the abstract state?** Adds complexity
  proportional to call-graph depth × number of bank-changing
  callees. May be cheaper to keep the wrapper-bypass detector and
  add per-call DB-claim runtime verification.
- **Should D be added to the abstract state?** Same trade-off.
  SMW's D=0 default means the cost is currently nil; another ROM
  might force the issue.
- **How do we detect a new gap class proactively?** Today: hit it,
  classify it, add an entry. Aspirational: run the runtime
  verifiers across the test corpus continuously and treat any new
  trip as a candidate gap.

## Change log

- 2026-05-17: initial inventory. Synthesized from
  `ABSTRACT_INTERPRETATION_GAPS.md`, `ENHANCEMENTS.md`,
  `ISSUES.md`, snesrecomp commit log, and SMW memory entries.
  ~45 entries across 11 categories. ~40 CLOSED, ~15 OPEN /
  PARTIAL / N/A.
