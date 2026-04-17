# SNES Recompilation – Claude Rules & Protocol

This project is a **static SNES recompilation system**.

We are NOT building an emulator.
We are NOT trying to "make it look close enough."

We are achieving **measured behavioral equivalence** using:
- the recompiled/native build
- an oracle implementation
- Ghidra for literal machine-code grounding

For Super Mario World work, the oracle implementation is typically **smw-rev**.
However, **smw-rev is not a perfect literal oracle**. It contains inferred / reconstructed code in some places.
Therefore:
- use **smw-rev for behavior comparison**
- use **Ghidra for literal code / control-flow / data interpretation**
- do NOT assume smw-rev is 1:1 machine-code truth

---

# 🔴 PRIMARY OBJECTIVE

We are building a SNES recompilation workflow that can correctly execute real commercial code.

For current work:
- Super Mario World is the working reference target
- smw-rev is the primary behavior oracle when applicable
- Ghidra is the primary literal-code oracle
- the recompiler, runtime, and tooling are all considered incomplete

If behavior is correct but the explanation depends on non-literal smw-rev helper logic, that MUST be acknowledged explicitly.

---

# 🔴 HARD RULES (NON-NEGOTIABLE)

If ANY rule is violated:
- the response is INVALID
- STOP immediately
- restart using the correct protocol

---

## 0. THE RECOMPILER IS THE AUTHORITY — CFG IS NOT

The recompiler (recomp.py + runtime) is the ONLY authoritative description
of how 65816 code becomes C. Every correctness claim must be something the
recompiler can derive from the ROM itself.

The `.cfg` files exist ONLY to supply information the recompiler truly
cannot reconstruct from the ROM alone. Concretely: data-region boundaries
(so bytes aren't decoded as code), function signatures where the calling
convention depends on external context (e.g. WRAM struct returns), and
rare hints that cannot be inferred statically.

This means:
- A `dispatch`, `skip`, `jsl_dispatch*`, or similar cfg hint is SUSPECT.
  If the recompiler can handle the pattern itself, the hint is obsolete
  and MUST be removed — not routed around in Python.
- A `name ADDR NAME sig:…` entry whose NAME is already defined as a
  `func` elsewhere is an alias for caller convenience, nothing more.
  Internally, the recompiler should resolve every call to a `func` —
  never to a bare `name`.
- When a cfg and the ROM disagree, the ROM wins. Fix the cfg (or delete
  the entry entirely) — never add recompiler code whose only purpose is
  to tolerate bad cfg data.
- When a cfg entry predates a recompiler feature that now makes it
  unnecessary, DELETE the entry. Redundant cfg is rot.

Whenever a fix is tempting in Python, ask: "is this really a recompiler
correctness fix, or am I working around wrong cfg?" If the latter,
delete the cfg line instead.

---

## 1. NO GUESSING

- No "likely"
- No "probably"
- No "might be"
- No speculative fixes

Every claim MUST be backed by measured data, Ghidra evidence, or explicit behavior comparison.

---

## 2. NO STDOUT DEBUGGING

- printf/logging is FORBIDDEN for primary debugging
- ad-hoc log spam is FORBIDDEN

All debugging must use structured tooling:
- TCP state inspection
- timeseries/ring buffer analysis
- targeted diff tooling
- Ghidra inspection
- smw-rev comparison where appropriate

---

## 3. ALWAYS USE AN ORACLE

You MUST compare against an oracle.

For SNES work there are TWO oracle types:

### A. Literal oracle
Use **Ghidra / ROM / disassembly truth** for:
- control flow
- instruction semantics
- register width state (m/x)
- banked addressing
- DMA/HDMA setup
- exact memory access interpretation
- inline data vs code
- long/short call behavior

### B. Behavioral oracle
Use **smw-rev** for:
- behavior comparison
- state comparison
- function-level comparison where mappings are valid
- visual/gameplay/output validation
- cross-checking higher-level intent

You MUST state which oracle is being used and why.

---

## 4. DO NOT TREAT SMW-REV AS PERFECTLY 1:1

smw-rev may:
- reconstruct logic
- rename and reorganize code
- introduce inferred helper behavior
- differ structurally while still behaving correctly

Therefore:
- do NOT assume source-level mismatch means bug
- do NOT assume source-level similarity means correctness
- use smw-rev primarily as a behavior oracle, not unquestioned literal truth

If a comparison depends on smw-rev not being literal, that MUST be stated explicitly.

---

## 5. FIX ROOT CAUSE ONLY

- No speculative fixes
- No symptom patching
- No "quick fix"
- No rewriting unrelated systems blindly

---

## 6. DO NOT TRUST THE SYSTEM

Assume ALL of the following may be wrong:
- recompiler
- runtime
- renderer
- DMA/HDMA timing
- bank mapping
- function boundaries
- dispatch/jump table interpretation
- smw-rev assumptions
- current tooling / observability

No subsystem is assumed complete.

---

## 7. FIX THE TOOL, NOT THE OUTPUT

- NEVER hand-edit generated output
- Fix:
  - the recompiler / generator
  - the runtime / hardware layer
  - the comparison tooling
  - the TCP/debug tooling

If generated code is wrong, fix generation.
If observability is insufficient, build the tools.

---

## 8. BUILD TOOLING WHEN MISSING

If a required observation does not exist:
- build it into the native TCP server
- build it into the smw-rev TCP server
- build comparison tooling
- then continue

You MUST NOT work around missing tooling with guesses or log spam.

---

# 🔴 REQUIRED DEBUGGING PROTOCOL

All debugging MUST follow the protocol in `DEBUG.md`.
All TCP/debug interface details are in `TCP.md`.

High-level summary:

1. Define target behavior precisely
2. Choose correct oracle (Ghidra, smw-rev, or both)
3. Establish sync point/state
4. Dump full relevant state from both sides
5. Validate completeness
6. Run timeseries analysis
7. Find first divergence
8. Trace cause
9. Classify bug
10. Apply minimal fix
11. Re-test from same sync point

If any step is skipped → STOP

---

# 🔴 ORACLE SELECTION RULES

## Use Ghidra FIRST when the question is about:
- what a routine literally does
- whether code vs data is misidentified
- function boundaries
- jump tables / dispatch tables
- stack / return-address tricks
- bank crossing
- m/x width state
- DP/DB/PB effects
- DMA/HDMA register programming
- IRQ/NMI entry/exit behavior
- exact register semantics

## Use smw-rev FIRST when the question is about:
- intended behavior
- gameplay state progression
- higher-level state progression
- visual/gameplay/output validation
- comparing equivalent high-level routines
- checking whether native behavior matches expected game outcome

## Use BOTH when:
- code and behavior both matter
- a function mapping exists but literal validation is still needed
- a visual issue may stem from codegen/runtime but decomp gives useful semantic landmarks

You MUST explicitly say:
- which oracle you are using
- what question it answers
- why that oracle is appropriate

---

# 🔴 SNES-SPECIFIC HIGH-RISK AREAS

These must never be hand-waved:

## 65816 CPU STATE
- m flag / accumulator width
- x flag / index width
- direct page
- data bank
- program bank
- emulation/native mode
- stack width assumptions
- REP/SEP transitions

## MEMORY / BANKING
- LoROM/HiROM mapping
- bank crossing
- WRAM vs VRAM vs CGRAM vs OAM
- long vs bank-local addressing
- mirror behavior if relevant

## PPU / VISUALS
- tilemaps
- BG mode state
- scroll registers
- CGRAM / palette
- OAM / sprites
- windowing / masking if relevant
- mosaic if relevant
- forced blank
- screen enable state
- VRAM address/increment mode

## DMA / HDMA
- DMA channel config
- source/dest interpretation
- transfer mode
- timing / ordering
- per-scanline HDMA effects

## INTERRUPTS / TIMING
- NMI
- IRQ
- VBlank timing
- HBlank timing when relevant
- latch/PPU timing assumptions

## AUDIO / APU
- do not hand-wave if issue intersects SPC/APU init, sync, or side effects

## DECOMP COMPARISON PITFALLS
- helper functions in smw-rev may not be literal
- function boundaries may differ
- source-level equivalence is not required
- behavior equivalence is what matters unless literal codegen issue is under inspection

---

# 🔴 REQUIRED TOOLING BEHAVIOR

There are typically TWO structured debug targets:

1. Native/recompiled runtime TCP server
2. smw-rev TCP/debug server

These are the PRIMARY debugging interfaces.

If either side lacks required state exposure:
- build it
- document it
- use it
- do not proceed blindly

Tooling MUST prefer:
- structured reads
- range/timeseries queries
- targeted diffs
- causality tracing

Tooling MUST NOT rely on:
- screenshot spam
- stdout spam
- hand-wavy visual guesses

---

# 🔴 FULL STATE REQUIREMENT

"Full state" is contextual, but for visual / gameplay / CPU debugging you MUST capture all relevant state for the subsystem.

At minimum, tooling should aim to expose:

### CPU / EXECUTION
- PC
- A, X, Y, S, D, DB, PB, P
- emulation/native mode
- m/x width state
- current function marker if native PC is not directly meaningful

### MEMORY
- WRAM
- relevant ROM mapping context
- direct page sensitive memory if applicable

### PPU
- VRAM
- CGRAM
- OAM
- BG mode / screen mode state
- scroll state
- tilemap relevant state
- VRAM increment / addressing state
- forced blank / screen enable
- mosaic/window state if issue may depend on it

### DMA / HDMA
- per-channel registers/state
- pending/active transfers
- transfer descriptors as applicable

### INTERRUPTS / TIMING
- NMI pending/servicing state
- IRQ pending/servicing state
- frame/scanline counters if available
- cycle budget / timing markers if available

### APU / AUDIO
- enough state to rule in/out APU interaction when relevant

If required state is missing:
- the dump is INVALID
- STOP
- build tooling first

Dumping only RAM is INVALID when the issue is visual.
Dumping only screenshots is INVALID.
Dumping only a guessed subset is INVALID.

---

# 🔴 TIMESERIES REQUIREMENT

Single-frame inspection is NOT sufficient.

You MUST:
- analyze a range
- identify when systems are still equivalent
- identify first divergence
- distinguish root divergence from later fallout

Timeseries is REQUIRED for:
- visual corruption
- tilemap issues
- DMA/HDMA issues
- NMI/IRQ issues
- startup/init issues
- scanline-dependent issues

---

# 🔴 NO WORKAROUNDS

If analysis is blocked:
- DO NOT guess
- DO NOT patch blindly
- DO NOT add compatibility hacks
- DO NOT fall back to crude logs

Instead:
- extend tooling
- add observability
- add structured diff support
- then continue

---

# 🔴 GHIDRA REQUIREMENT

Before analyzing any unknown SNES code behavior:

- ensure the ROM is correctly loaded in Ghidra
- use Ghidra to inspect the literal instructions
- confirm bank/context
- confirm m/x state assumptions
- confirm whether data/code boundaries are valid

Never guess 65816 behavior from appearance alone.

---

# 🔴 PROCESS RULES

- Kill all relevant instances before relaunching
- No screenshot spam
- Use targeted scripted screenshots only when paired with state diffs
- If native PC is not directly stable, ensure a meaningful function/trace marker exists
- When smw-rev and native are both instrumented, prefer structured comparisons over manual reading

---

# 🔴 SESSION START REQUIREMENT

Before doing ANY work, Claude MUST explicitly state:

1. I have read `CLAUDE.md` and `DEBUG.md`
2. I will not guess
3. I will use Ghidra for literal code truth
4. I will use smw-rev as a behavioral oracle where appropriate
5. I will explicitly state which oracle I am using and why
6. I will use structured TCP/debug tooling
7. If tooling is missing, I will build it first
8. I will identify first divergence before proposing a fix

If this acknowledgement is missing → STOP
