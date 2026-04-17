# DEBUG_SNES.md — SNES Debugging Protocol

This file defines the REQUIRED debugging workflow for SNES recompilation work.

Follow it exactly.
If any step is skipped, the analysis is INVALID and must restart.

---

# 🔴 RULE 0 — LOADSTATE IS FORBIDDEN

Do NOT use `loadstate` on either server under any circumstances until further notice.
Loading states desynchronizes the two servers and destroys the debugging session.
If you need a fresh state, kill both processes and relaunch from scratch.

---

# 🔴 CORE PRINCIPLE

This is a **verification problem**, not a “make it mostly work” problem.

We are trying to identify the **first measured divergence** between:
- native/recompiled execution
- behavioral oracle execution (usually smw-rev)
- literal machine-code truth (via Ghidra) when needed

All fixes must come from measured divergence.

---

# 🔴 PRIMARY GOAL

For current work, we are using Super Mario World as the reference target.

That means:
- compare native behavior against smw-rev behavior where mappings are meaningful
- use Ghidra to resolve literal code truth
- be explicit when smw-rev is not literal enough to answer the question
- never confuse "source similarity" with correctness

---

# 🔴 HARD REQUIREMENTS

## 1. NO GUESSING
- Do not speculate
- Do not say "likely", "probably", or "might be"
- Every claim must be supported by data, Ghidra evidence, or explicit behavior comparison

## 2. NO STDOUT DEBUGGING
- No printf spam
- No ad-hoc logs as primary debugging
- No giant unstructured trace dumps unless explicitly building a structured tool from them

All debugging must use structured observability through TCP tooling, targeted diffs, and oracle comparisons.

## 3. NO WORKAROUNDS
If required observability is missing:
- build the tool
- expose the state
- retry

Do not work around missing visibility.

## 4. FIX THE TOOL, NOT THE OUTPUT
- Never hand-edit generated output
- Fix the recompiler, runtime, comparison tooling, or debug tooling

## 5. EXPLICITLY CHOOSE THE ORACLE
At the start of analysis, state whether the question is best answered by:
- Ghidra
- smw-rev
- both

If this is not stated, STOP.

---

# 🔴 REQUIRED DEBUGGING LOOP

## STEP 1 — DEFINE THE TARGET BEHAVIOR

Before debugging, state exactly what behavior is being validated.

Examples:
- title screen BG3 tilemap is populated correctly
- NMI-driven VRAM upload matches expected progression
- sprite OAM contents match behavioral oracle
- HDMA setup matches intended per-scanline effect
- function XYZ produces equivalent WRAM side effects

If expected behavior is not clearly defined, STOP.

---

## STEP 2 — CHOOSE THE CORRECT ORACLE

Explicitly state:

### Use Ghidra if the main question is:
- what the ROM literally does
- how an instruction sequence behaves
- whether a function boundary is real
- whether a call/jump target is real
- width state / bank state / addressing semantics
- DMA/HDMA programming details
- interrupt entry/exit details

### Use smw-rev if the main question is:
- intended gameplay behavior
- higher-level state progression
- equivalent function output when mappings are known
- verifying what "correct" behavior should look like over time

### Use both if:
- code and behavior both matter
- a function mapping exists but literal validation is still needed
- visual corruption needs both semantic landmarks and hardware-state tracing

If oracle choice is ambiguous, STOP and resolve it first.

---

## STEP 3 — ESTABLISH A SYNC POINT

Native and oracle states may not align by frame number.

You MUST sync using a comparable execution state, such as:

- PC / function marker
- register set including m/x width state
- bank context
- specific WRAM landmarks
- VRAM/CGRAM/OAM snapshot features
- DMA/HDMA state
- NMI/IRQ phase markers
- specific gameplay state markers

Frame number alone is NOT sufficient.

If sync cannot be established:
- STOP
- explain what state is missing
- build tooling if needed

---

## STEP 4 — DUMP FULL RELEVANT STATE FROM BOTH SIDES

You MUST capture state from:
1. native runtime TCP server
2. smw-rev TCP/debug server

And when needed:
3. Ghidra-confirmed literal interpretation context

For visual issues, this generally means you need far more than RAM.

---

# 🔴 FULL STATE CHECKLIST

## CPU / EXECUTION
- PC or meaningful function marker
- A, X, Y, S, D, DB, PB, P
- emulation/native mode
- m/x width state

## MEMORY
- WRAM
- relevant ROM/bank context
- stack/direct-page relevant regions when needed

## PPU / VISUAL
- VRAM
- CGRAM
- OAM
- BG mode state
- tilemap regions relevant to issue
- scroll registers
- screen enable / forced blank
- VRAM increment/address state
- mode 7 / windows / mosaic state if relevant

## DMA / HDMA
- all relevant channel registers/state
- source/dest/mode
- active transfer info
- per-scanline tables if relevant

## INTERRUPTS / TIMING
- NMI state
- IRQ state
- frame/scanline markers if available
- timing counters if available

## APU / AUDIO
- enough state to rule in/out audio-side coupling if relevant

---

If required state is missing:
- the dump is INVALID
- STOP
- build the missing tooling first

Dumping only RAM is INVALID for visual bugs.
Dumping only screenshots is INVALID.

---

## STEP 5 — VALIDATE COMPLETENESS

Before analysis, explicitly confirm:

- CPU state present
- memory state present
- PPU state present
- DMA/HDMA state present if relevant
- interrupt/timing state present if relevant
- APU state present if relevant
- oracle choice still valid

Do NOT proceed with partial state.

---

## STEP 6 — RUN A TIMESERIES ANALYSIS

Single-snapshot debugging is not enough.

You MUST analyze a RANGE and determine:
- when systems are still equivalent
- first divergence point
- which subsystem diverges first
- whether later differences are consequences

Timeseries is REQUIRED for:
- visual corruption
- startup/init issues
- VRAM/CGRAM/OAM issues
- DMA/HDMA issues
- NMI/IRQ issues
- frame-to-frame state progression issues

If only one snapshot is used, STOP.

---

## STEP 7 — PRODUCE AN EXACT DIFF

You MUST produce a concrete diff.

Example formats:

Sync basis: function marker XYZ, BG mode 1, equivalent NMI phase

First divergence:
- Subsystem: VRAM
- Address: 0x1234
- Expected: 0x56
- Actual:   0x00

or

- Subsystem: CPU state
- Register: DB
- Expected: 0x7E
- Actual:   0x00

or

- Subsystem: DMA
- Channel: 2
- Register: A1T/A1B/DMAP
- Expected: ...
- Actual:   ...

If you cannot produce an exact diff, STOP.

---

## STEP 8 — TRACE THE CAUSE

You MUST identify:
- function
- instruction or state transition
- subsystem
- write / transfer / event causing divergence
- control path or call path
- whether divergence is immediate or timing-derived

For literal code interpretation questions, confirm with Ghidra.
For behavior-only comparisons, make sure smw-rev is appropriate.

If the cause cannot be traced:
- STOP
- extend tooling
- retry

---

## STEP 9 — CLASSIFY THE BUG

Classify the bug as one of:

- Codegen bug
- Runtime/hardware simulation bug
- Timing bug
- Function-boundary / discovery bug
- Oracle-mismatch / smw-rev-nonliteral issue
- Tooling/observability gap

Pick the PRIMARY cause.
Do not blur categories lazily.

---

## STEP 10 — APPLY THE MINIMAL FIX

Only after exact cause is proven:
- apply the smallest correct fix
- modify only the responsible subsystem
- do not patch downstream symptoms
- do not add compatibility hacks
- do not mutate generated output manually

Then re-run the same sync + timeseries + diff workflow.

---

# 🔴 REQUIRED RESPONSE FORMAT

Every debugging response must include:

1. Target behavior
2. Oracle choice and why
3. Sync basis
4. State captured from native
5. State captured from smw-rev
6. Ghidra evidence used (if any)
7. Completeness validation
8. Timeseries range analyzed
9. First divergence
10. Trace of causal write/state transition
11. Classification
12. Minimal fix proposal (only if justified)
13. Re-test plan

If any section is missing, STOP.

---

# 🔴 REQUIRED TOOLING BEHAVIOR

Authoritative structured targets:
1. native runtime TCP/debug interface
2. smw-rev TCP/debug interface

All serious analysis must go through these systems.

Required capabilities should include:
- arbitrary memory reads
- CPU register/state reads
- VRAM/CGRAM/OAM dump
- DMA/HDMA state query
- interrupt/timing state query
- timeseries/ring buffer queries
- trace of writes/state transitions over time
- stable function/execution markers if native PC is insufficient

If a required capability is missing:
- build it
- do not work around it

## 🔴 RING BUFFER COVERAGE RULE

If a variable needed for analysis is **not exposed by the ring buffer**, you MUST:

1. **STOP immediately** — do not attempt to reason around missing data
2. **Inform the user** — state exactly what address/state is missing and why it's needed
3. **Wait for instructions** — the user decides whether to extend the ring buffer, use a different approach, or deprioritize

No exceptions. Do not:
- Guess what the value "probably" is
- Read current-state RAM and assume it reflects historical frames
- Reason about the value from surrounding code without measured data
- Build ad-hoc workarounds (e.g., restart + trace_addr) without user approval

The ring buffer is the primary historical debugging mechanism. If it has blind spots, that is a tooling gap — treat it like any other missing observability.

## 🔴 COMPARISON TOOL PROTOCOL

When comparing state between recomp and oracle:

1. **Do it by hand first** — query both ring buffers directly, read the data, and compare yourself. This validates your understanding of what to look for and catches tool bugs early.
2. **Only after hand-validation succeeds**, build a Python script to automate the comparison for repeated use.
3. **Validate the script** — run it and confirm its output matches your manual comparison before relying on it.

Do not:
- Build comparison scripts before understanding the data
- Trust script output without at least one manual cross-check
- Accumulate one-off throwaway scripts — if a script is not reusable, delete it

---

# 🔴 SNES VISUAL DEBUGGING WATCHOUTS

Visual issues are especially high-risk.

Do NOT assume a visual bug is "just rendering."

Possible roots include:
- bad function discovery
- wrong m/x width handling
- wrong DB/PB context
- VRAM upload bugs
- tilemap write bugs
- CGRAM palette bugs
- OAM bugs
- DMA/HDMA timing/order bugs
- NMI timing/order bugs
- renderer bugs

For visual bugs, screenshots may help orient the issue, but screenshots alone are NEVER sufficient.

You need:
- visual state
- memory state
- transfer state
- timing state
- timeseries

---

# 🔴 SESSION START CHECKLIST

Before doing any work, Claude must explicitly state:

- I have read `CLAUDE.md` and `DEBUG.md`
- I will not guess
- I will explicitly choose the right oracle
- I will use Ghidra for literal machine-code truth
- I will use smw-rev as a behavioral oracle where appropriate
- I will use structured TCP/debug tooling
- If tooling is missing, I will build it first
- I will identify first divergence before proposing a fix

If this acknowledgement is missing, STOP.

---

# 🔴 DIVERGENCE DEBUGGING HEURISTICS (LEARNED)

## 1. Classify data vs runtime before investigating codegen

When visual data (tilemap, Map16) is correct but behavior (collision, physics) is wrong, immediately split:
- A) Data generation wrong (Map16 low bytes, high bytes, VRAM)
- B) Runtime lookup/interpretation wrong (collision check, pointer setup)

Do NOT inspect codegen until classification is established.

## 2. Single-cell tracing before full-table analysis

Before diffing entire Map16 tables or memory regions:
- Pick ONE exact failing cell
- Trace its write history on both sides (trace_addr)
- Compare the write sequences
- Identify whether the write is missing, wrong value, or wrong address

This avoids premature broad analysis.

## 3. Reduce to first divergence within a single object

When a diff shows many cells wrong, identify the FIRST cell that diverges. Trace that one cell. Do not scatter across multiple objects or subsystems.

## 4. Watch for recompiler register propagation in loops

When a function returns a register value (e.g., ret_y) inside a loop, verify that the return value is assigned back to the loop variable before the backward branch. The recompiler may fail to propagate registers at back-edges, especially in nested loops.

## 5. Shared label variables from ret_y can cause sync deadlocks

When a ret_y function sets both A and Y to the same return value, backward branch register sync guards may block BOTH A and Y propagation. If both label variables are the same (la == ly), neither sync runs. This is a known recompiler edge case — check generated code for missing variable assignments before loop gotos.

## 6. Mode 0x07 IS the attract demo

The title screen attract demo (with level visuals, ground, sprites, Mario walking) runs in game mode 0x07. It does NOT require mode 0x14 or any input. Start debugging immediately once mode 0x07 is reached (~frame 200).

## 7. Map16 data persists across title screen cycles

The Map16 table is written during the first level load and is NOT rewritten during subsequent title screen mode cycles. To trace Map16 writes, set up trace_addr BEFORE the first level load (before mode 0x05 at ~frame 95), not after.

## 7b. Attract-demo death invalidates everything after it (CRITICAL)

**Mario dying in the attract demo is itself a bug, not just a cutoff.**

The attract demo is a recorded auto-play that the original SMW ROM
completes without dying. If Mario dies in our recomp run, that means
*upstream* state was already wrong before the death — wrong environment,
wrong enemy spawns, wrong physics, wrong collision, wrong scroll, wrong
sub-pixel positioning, etc. The death is the *consequence*, not the
problem. Fixing post-death symptoms is the wrong layer; the real fix is
to find what diverged BEFORE the death and correct that.

**Symptom (mechanical):** when running the attract demo with corrected
jump physics, Mario eventually falls off the map and dies. The attract
demo recording has no concept of death — the game responds by respawning
Mario into **real gameplay**, leaving the attract demo entirely. From
that frame onward the recomp is no longer in the mode 0x07 / mode 0x14
attract sequence, it's in player-controlled gameplay with no input,
doing semi-arbitrary things.

**Rule:** any state captured AFTER the first attract-demo death is
INVALID for attract-demo parity comparison. Discard it. New violations,
divergences, or "bugs" observed past that point are NOT real bugs in the
attract demo path — they're symptoms of the game running in an unintended
post-death control loop. The path forward is to push the divergence
detection EARLIER until we find the upstream cause that led to Mario
falling/dying in the first place.

**How to detect the boundary:**
- the attract demo lifetime is bounded; instrument the recomp to log the
  frame number where Mario's life count decrements OR where mode
  transitions out of `0x07/0x14` while still inside the attract demo
- treat that frame as the cutoff. Anything later is post-death and must
  be discarded
- if a Rule20 violation, divergence, or visible glitch only fires past
  this cutoff, do NOT chase it. The fix belongs upstream (before the
  death) or in a separate "actual gameplay" debugging session

**Why this matters for Rule20 work:** my Rule20 elimination passes need
to focus on what crashes the recomp BEFORE the death frame. Anything
that crashes after death is on a code path the attract demo was never
supposed to enter and may be unreachable in legitimate gameplay too.

## 8. Cross-bank JSL Y-preservation (solved 2026-04-06)

**Symptom:** Rule20 `Y_unknown_index` violations at `STA $xxxx,Y` / `LDA $xxxx,Y` instructions immediately after a cross-bank `JSL $XX:YYYY`, even when the callee demonstrably does not touch the Y register.

**Root cause:** `recomp.py` is invoked per-bank. Its `y_modifies_set` was built by decoding only the current bank's cfg functions. For every cross-bank `JSL` target the set was therefore empty, so the emitter fell back to a conservative *"if `y_modifies_set` is non-empty and `callee_bank != self.bank`, clobber Y"* rule. That fallback unconditionally discarded Y across **every** cross-bank call, regardless of whether the callee actually touched Y.

Also affected: single-instruction entry points that fall-through into a `ret_y`-declared sub-entry (e.g. `$0D:A95B` → `$0D:A95D` `_Entry2`). The decoder correctly emitted a fall-through tail call (`return _Entry2(j)`), but the parent function's own sig did not inherit `ret_y`, so callers believed Y was clobbered even though the tail call returned Y.

**Fix (recomp.py):**

1. New helper `_compute_global_y_state(rom, cfg_dir)` loads every `bank*.cfg` in the cfg directory, decodes all functions in all banks, and builds:
   * a **global `y_modifies`** set keyed by full `(bank<<16)|addr`, using the same LDY/TAY/INY/DEY/PLY/TXY check + PHY/PLY bracketing detection as before
   * a **global `ret_y_funcs`** set seeded from explicit `ret_y` cfg tokens, then expanded to a fixed point via tail-call / fall-through propagation (unconditional `JMP` / `JML` / `BRA` / `BRL` to a `ret_y` target, or non-terminator last instruction falling through to a `ret_y` next cfg function)
2. `run_config` now accepts `cfg_path`, calls the helper once per invocation (results cached per cfg_dir in-process), and:
   * seeds the per-bank `y_modifies_set` from the global set
   * merges propagated `ret_y_funcs` into `cfg.ret_y_funcs` before emission
3. The conservative cross-bank clobber at the JSL emission site is removed. The condition is now a plain `target in self.y_modifies` (global), with no fallback.

**Detection method for similar bugs:**

When Rule20 violations cluster around a single register kind (`Y_unknown_*`, `X_unknown_*`), do NOT fix them one-by-one via cfg hints. Instead:

1. Run the binary with the abort logger and capture the **first** violation hit (`_rule20_die` already prints addr + kind + recomp call stack).
2. Disassemble the violating address in Ghidra and trace backward to the last instruction that set the missing register.
3. Identify the call(s) between that write and the violation. For each call, check whether the recompiler's liveness model thinks the register is clobbered, and whether that matches the callee's actual literal 65816 code.
4. If there is a mismatch, the fix is almost certainly a recompiler-level liveness / sig-propagation gap, **not** a missing cfg hint. Fix the generator.

**Measured impact (attract demo path):**

* Total Rule20 violation sites: 335 → 225 (−33%)
* `Y_unknown_index` specifically: 297 → 190 (−36%)
* First runtime crash advanced past `$0D:B5E5` (`GrassObj3F_SmallBushes`, intra-bank fall-through iterator case) to `$0D:A102` (a separate class: intra-function sub-routine with an over-specified auto-generated `(uint8_a, uint8_k)` sig on a callee that overwrites both A and X before reading them — out of scope for this fix).

**Remaining `Y_unknown_index` sites are a different class:** standalone iterator / slot-scanner functions (e.g. `CheckAvailableScoreSpriteSlot` at `$02:AD34`, `InitializeBlockPunchAttack` at `$02:86ED`) whose first instruction is `LDY #imm` and which return the scan result in Y, but whose auto-generated cfg sig lacks `ret_y`. These need either a cfg-gen improvement (detect scanner pattern) or a recompiler-level heuristic (if a function's RTS paths all reach a TAY/LDY and caller indexes with Y, infer `ret_y`).
