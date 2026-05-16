# Decoder soundness reference

**Audience:** snesrecomp contributors who modify the v2 decoder, codegen,
or any consumer of decoded `(m, x)` state. Read this BEFORE shipping any
feature that depends on the decoder's static M/X being correct.

**Status:** living document. Each new soundness-dependent feature is
audited against this enumeration before landing.

## Why this exists

The v2 decoder performs **abstract interpretation** over the 65816's
processor-status flags. The abstract domain is a 4-element lattice:

```
(M, X) ∈ {0, 1} × {0, 1}
```

…augmented with a bounded stack of PHP-pushed (M, X) snapshots (added
2026-05-15 as `DecodeKey.p_stack`):

```
state = (M, X, p_stack)
p_stack ∈ list of (M, X), |p_stack| ≤ 8
```

Every 65816 instruction has a **transfer function** that maps an input
state to an output state. The decoder evaluates this function during
its worklist walk. When the function is **sound** — i.e., it matches the
runtime semantics on every reachable path — downstream consumers can
trust the decoded `(M, X)` for static decisions (operand width pinning,
exit-state inference, dead-code elimination).

When the function is **unsound** at some node — i.e., it claims a state
the runtime doesn't actually reach — consumers that depend on it
produce wrong code at that node.

Two major unsoundness bugs have already been caught + closed:

- **2026-05-03 (RunPlayerBlockCode m=1 lost across JSR)** — the decoder
  assumed callees preserved M/X across `JSR`/`JSL`. Wrong when the
  callee internally executes SEP/REP without restoring. Fixed by the
  `callee_exit_mx` lookup (cfg `exit_mx_at` directives + the leaf
  auto-router).

- **2026-05-15 (UpdateSaveBuffer PLP-restored width mismatch)** — the
  decoder assumed PHP/PLP didn't affect M/X. Wrong: PHP pushes P (which
  contains M/X), PLP restores it. The static-width pinning of
  PHA/PHX/PHY/PLA/PLX/PLY in `1e476e6` then locked in the wrong post-
  SEP widths at PLP-restored PLX/PLY sites, producing -2 stack drift on
  the overworld save path. Fixed by adding `p_stack` to `DecodeKey` +
  PHP/PLP transfer-function semantics in `post_state`.

Both bugs had this common shape: the decoder's transfer function did
not model some runtime M/X side-channel, and a consumer (`callee_exit_mx`
in the first, `1e476e6` static-width pinning in the second) shipped
expecting soundness. Symptoms surfaced days later as user-visible
glitches in unrelated-looking gameplay regions.

**The whackamole-prevention principle:** every runtime side-channel into
M/X must either be modeled by the decoder's transfer function, OR
explicitly marked ambiguous (`m=None` / `x=None`) so consumers fall back
to runtime checks. Silently-wrong static state is the failure mode.

## Runtime side-channels into M/X — the enumeration

| 65816 op / channel | Effect on M/X at runtime | Decoder handling | Risk if unhandled |
|---|---|---|---|
| `REP #imm` | Clears M/X bits per immediate mask | ✓ modeled in `post_state` | n/a |
| `SEP #imm` | Sets M/X bits per immediate mask | ✓ modeled in `post_state` | n/a |
| `PHP` | Pushes P (M/X live inside it); does not mutate M/X | ✓ modeled (2026-05-15): pushes (m, x) onto `p_stack` | (was: PLP-restore invisible to analyzer; caused UpdateSaveBuffer bug) |
| `PLP` | Pops P from stack → restores M/X to pushed snapshot | ✓ modeled (2026-05-15): pops `p_stack`, restores (m, x) | (was: this gap) |
| `RTI` | Pulls P off stack (interrupt-return) → restores M/X to whatever the interrupt-entry pushed | ✗ NOT modeled. Treated as a terminator; no successor decoded. | Low for SMW: NMI/IRQ handlers are decoded as **separate top-level cfg entries** with declared entry M/X. The handler's *return* via RTI is not analyzed as a successor in the same function. **Confirm:** any handler that does work AFTER its RTI in the same decoded body would be wrong. SMW doesn't appear to do this. |
| `XCE` | Exchange Carry ↔ Emulation flag. If E becomes 1, M and X are forced to 1; D is forced to 0. | ✗ NOT modeled. M/X stay at their pre-XCE values in the abstract state. | Low: SMW executes XCE once at I_RESET (forces native mode), never again. Boot-only risk. |
| `BRK` / `COP` | Software interrupt: pushes PB, PC, P; jumps to vector. | ✓ Treated as terminators (no successor). | Low: SMW doesn't use BRK/COP in normal flow. Decoder emitting `/* BRK */` early-return at a phantom BRK is how the Bug C visual cascade surfaced. |
| `WAI` / `STP` | CPU halt. | ✓ Treated as terminators. | n/a |
| `JSR` / `JSL` | Caller's M/X persists across the call **iff** callee preserves M/X. Callee can mutate via internal SEP/REP/PHP/PLP. | Partial: caller assumes preservation unless `callee_exit_mx` overrides. `callee_exit_mx` is populated from cfg `exit_mx_at` directives (manual) + leaf auto-router (limited to non-JSR/JSL functions). | **Significant**: non-leaf functions whose exit depends on entry variant are not auto-routed. Mitigated by per-variant exit_mx (currently reverted, retry in progress). |
| `RTS` / `RTL` | No effect on M/X. | ✓ Treated as terminators. | n/a |
| Hardware NMI / IRQ entry (CPU-driven, not an instruction) | Pushes P + PC, sets I=1, forces native-mode (m, x) at vector | ✓ Handlers decoded as separate cfg entries with declared `entry_m, entry_x`. The hardware-pushed P is restored via RTI which is treated as a terminator. | n/a — handlers are stand-alone. |
| Self-modifying code (WRAM / SRAM execution) | Bytes change at runtime; abstract analysis can't see the runtime version. | ✓ Decoder rejects WRAM/SRAM-resident code addresses. SMW has one WRAM-resident routine (DecompressTo inner loop) which is HLE'd in `src/gen_stubs.c`. | n/a (HLE'd). |
| Indirect dispatch (`JMP (abs, X)`, `JML [abs]`) | Runtime resolves the target; target's entry M/X may differ from the call site. | Partial: dispatch-helper detection (`classify_dispatch_helper`) handles SMW's `ExecutePtr` trampolines and rewrites them to a synthesised C switch + direct call, forcing post-trampoline `(m=1, x=1)` per SMW's trampoline contract. Generic indirect dispatch beyond ExecutePtr is treated as a no-successor terminator (cfg-required-dispatch-or-kill rule). | Low for SMW; brittle for other ROMs with different trampolines. |
| Stack manipulation that synthesises a return PC (`PEA`/`PEI`/`PER` + `RTL`/`RTS`) | Pushed PC may resume at code with different M/X than where the synthesis happened. | ✗ NOT modeled at the abstract-M/X layer. Handled at the trampoline-pattern layer (above). | Low: SMW uses this pattern only in ExecutePtr-style dispatchers, which the dispatch-helper detector catches. |
| Hardware-level register write to `P` direct? | Not a thing in 65816. P is only modified by REP/SEP/PHP-PLP/RTI/XCE/branch+SEC/CLC/CLI/SEI/CLV/CLD/SED. The CL?/SE? flag ops touch C, I, V, D, N, Z but NOT M/X (those are SEP/REP only). | ✓ Implicitly handled — non-M/X flag ops don't change `(m, x)`. | n/a |

**Remaining significant gaps after 2026-05-15:**

1. **Non-leaf `callee_exit_mx` inference**. The leaf auto-router skips
   any function with a JSR/JSL. Functions like
   `PlayerState00_00F9C9` that do SEP #$20 before RTS-after-JSR are
   covered only by manual cfg `exit_mx_at` directives. A non-leaf
   inference pass — using a topologically-ordered fixpoint over the
   call graph — would close this. Two prior attempts regressed
   `GraphicsDecompress` (2026-05-03), reverted to opt-in. With the
   PHP/PLP tracker landed, the regression mechanism may no longer
   exist; re-attempt is in progress on `next/donut-plains-and-beyond`.

2. **Per-variant `callee_exit_mx` broadcast bug**. Even the leaf
   auto-router records exit as a single tuple per function and
   broadcasts to all four entry variants. Wrong for functions where
   exit depends on entry (e.g., REP-only mutators where X is
   preserved). Currently mitigated by manual cfg hints
   (`exit_mx_at 008b2b 0 0` for the M0X0 caller). A per-variant
   `exit_mx_at_per_variant` schema (attempted in 43c0cc6, reverted)
   would close this. Retry in progress.

3. **PHP/PLP across function calls.** The current model assumes a
   function's `p_stack` is internal — JSR/JSL preserves the caller's
   `p_stack` and the callee's PHPs/PLPs don't leak. This is correct
   for well-balanced callees. Unbalanced callees (PHP without matching
   PLP, or PLP without matching PHP) are not modeled. SMW does not
   appear to have such code. **Confirm if porting to other ROMs.**

## Existing consumers of the abstract `(M, X)` state

When adding a new consumer, audit it against the gaps above. Each
consumer below is annotated with which gaps it's sensitive to and how
it handles them.

### `cpu_trace_func_entry(cpu, pc24, name)` — runtime trace

**Sensitivity:** none — this is observability, not behavior.
**Safe:** ✓

### `_emit_pushreg` / `_emit_pullreg` (codegen.py, post-2026-05-15)

**Source:** snesrecomp commit `1e476e6` (PHA/PHX/PHY/PLA/PLX/PLY width
pinning to decoder's static `insn.m_flag` / `insn.x_flag`).

**Sensitivity:** depends on the decoder being sound at the push/pull
PC's static (M, X).

**Currently safe on:**
- REP/SEP-only paths ✓
- PHP/PLP-bracketed paths ✓ (since 2026-05-15)

**Currently sensitive to:**
- RTI's restored P (not modeled) — risk if a function body decodes
  past an RTI back into the same function. SMW doesn't do this.
- XCE's mode switch (not modeled) — risk only at I_RESET in SMW.

**Recommendation:** safe for SMW as of 2026-05-15. Audit again if
porting to a ROM that uses RTI mid-function or repeats XCE.

### `_emit_writereg` (codegen.py, post-2026-05-15)

**Source:** snesrecomp commit `d995ca2` (LDA/LDX/LDY/ALU/Shift A-mode
width pinning).

**Sensitivity:** same as `_emit_pushreg` / `_emit_pullreg`.

**Currently safe on:** same as above.

### Dispatch synthesis `_emit_dispatch` (codegen.py)

**Source:** snesrecomp commit `7da1cfe` (forces variant `_M1X1` + emits
SEP #$30-equivalent before the synthesised C switch).

**Sensitivity:** ROM-specific contract — assumes the trampoline ends in
SEP #$30 (true for SMW's ExecutePtr / ExecutePtrLong).

**Currently safe on:** SMW ✓.
**Not safe on:** other ROMs without verifying their trampoline
contract.

### `exit_mx_autoroute.detect_and_route` (leaf auto-router)

**Source:** snesrecomp commits `14c8eea` (pass 1), `8639d79` (pass 2),
ongoing.

**Sensitivity:** decodes only the cfg-declared entry variant (pass 1)
or all four variants requiring exit convergence (pass 2). Records a
single 4-tuple `(bank, addr16, exit_m, exit_x)` per function.

**Soundness gap:** the recorded tuple is **broadcast** to all four
entry variants at `v2_regen.py` time. For functions where exit depends
on entry, the broadcast over-claims for non-default variants. Bug C
visual (Iggy boss arena platform invisible) and the
`exit_mx_at 008b2b 0 0` cfg hint are direct consequences.

**Mitigation:** manual cfg hints. The per-variant retry on
`next/donut-plains-and-beyond` would close this.

### `callee_exit_mx` map (decoder.py JSR/JSL handler)

**Source:** ongoing. Populated by cfg `exit_mx_at` + auto-router.

**Sensitivity:** consumes the broadcast tuples from the auto-router.

**Soundness gap:** inherits the per-variant broadcast issue. Plus,
non-leaf functions are not auto-routed.

## How to verify a new consumer is safe

Before landing a feature that depends on the decoder's static `(M, X)`:

1. **Identify which 65816 ops your consumer is sensitive to.** If the
   consumer reads `insn.m_flag` or `insn.x_flag` for any op, list the
   ops.

2. **Cross-reference with the table above.** For each op, confirm the
   decoder models it correctly.

3. **For partial-coverage ops (JSR/JSL, indirect dispatch, etc.)**,
   verify that the cfg/auto-router covers the call sites your
   consumer touches.

4. **Run the runtime static-claim verifier** (see
   `RUNTIME_VERIFIER.md` once it exists) against a known-playable
   region (currently up to Yoshi's Island). The verifier should not
   trip. If it does, you've hit a latent gap that's not yet
   user-visible.

5. **Add a test** that exercises your consumer through a known-safe
   path. Include the path through every M/X-mutating op family your
   consumer touches.

## How to add a new ROM

If porting snesrecomp to a different SNES ROM, audit:

1. The trampoline contract assumed by `_emit_dispatch` (currently
   hard-coded to ExecutePtr's SEP #$30 → m=x=1 post-state).

2. Whether the ROM uses any 65816 op the decoder doesn't model
   (specifically RTI mid-function, repeated XCE, unbalanced PHP/PLP).

3. Whether the ROM has self-modifying code that needs HLE.

## Open questions

- **Is `p_stack` depth 8 enough?** Bounded defensively. SMW typical
  is 0–1. Worth measuring at regen time and flagging if any function
  needs more.

- **Should `RTI` mid-function be supported?** Adds a `p_stack`-like
  "interrupt-entry P snapshot" channel. Not yet needed for SMW. Adds
  state-space complexity proportional to interrupt-nesting depth.

- **What about user-data-driven indirect calls** (e.g., spawning a
  sprite whose handler is a function pointer table looked up at
  runtime)? Current behavior: decoder treats `JMP (abs, X)` as
  unauthorised, requires cfg `indirect_call_table` directive. This is
  the "cfg-required-dispatch-or-kill" rule.

## Change log

- 2026-05-15: PHP/PLP modeled in `post_state`; `p_stack` added to
  `DecodeKey`; `_dedupe_by_pcmx` post-pass merges variants. Snesrecomp
  `73e3d26`.
- 2026-05-03: `callee_exit_mx` added; cfg `exit_mx_at` directive
  introduced. Snesrecomp `bf2c147` and subsequent.
- 2026-05-02: Cross-function-block import + DB=$C0 root-cause fix.
  Inline-cross-fn-blocks. Snesrecomp earlier.
