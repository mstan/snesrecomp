<p align="center">
  <img src="docs/assets/snesrecomp-logo.png" alt="SNES Recomp" width="640">
</p>

# snesrecomp

A static recompiler for SNES (Super Famicom) games. Translates 65816
machine code into native C ahead-of-time, so the recompiled game runs
as a normal binary rather than under interpretation.

> ## Status: alpha, three games at varying playability
>
> **Super Mario World is believed fully playable** through the
> recompiler. **Mega Man X is believed fully playable** end-to-end.
> **A Link to the Past** is playable through the early dungeon. The
> framework itself is still pre-release: APIs change without warning,
> internal docs assume active-session context. Treat what you see here
> as a working snapshot, not a release.

## Per-game runner repos

snesrecomp is the shared framework. Each game lives in its own
companion repo that junctions this checkout as a sibling and supplies
the game-specific runtime, `.cfg`, and build glue.

- **Super Mario World** —
  [mstan/SuperMarioWorldRecomp](https://github.com/mstan/SuperMarioWorldRecomp).
  **Believed playable end-to-end.** Hand-verified through Worlds 1–2
  (Yoshi's Island + Donut Plains, including the Iggy castle boss) and
  into World 3 (Vanilla Dome); later worlds + special content not yet
  hand-verified but expected to play similarly. Two always-on runtime
  tripwires (M/X claim verifier and async cpu->m_flag/x_flag-write
  detector) have not latched on the verified worlds.

- **The Legend of Zelda: A Link to the Past** —
  [mstan/ZeldaAlttPSNESRecomp](https://github.com/mstan/ZeldaAlttPSNESRecomp).
  **Playable through early dungeon.** Boot → attract demo → file
  select → overworld → Module 07 (Dungeon) with sword combat is
  hand-verified. Later content not yet hand-verified.

- **Mega Man X** —
  [mstan/MegaManXSNESRecomp](https://github.com/mstan/MegaManXSNESRecomp).
  **Believed playable end-to-end.** Boot → Capcom logo → attract intro
  → title screen → intro stage and the Maverick stages with their
  bosses play through; the earlier lockups (cooperative-scheduler
  stalls, dispatch-site m/x mistranslations) and visual rough edges
  have been resolved.

The intent is for snesrecomp to be **game-agnostic** — adding a new
game should cost mostly per-game `.cfg` work, not months of framework
patching. SMW exercised the framework hard during 2026-04/05 and
surfaced the bug classes the framework now handles permanently:
per-variant exit-(M, X) inference with order-independent fixpoint,
dispatch-terminator JSL recognition, PHP/PLP-bracketed M/X tracking,
wrapper-bypass autorouter, tail-call autorouter, and a full runtime
tripwire suite catalogued in [`docs/TRIPWIRES.md`](docs/TRIPWIRES.md).
LttP and MMX have each surfaced their own framework gaps (LoROM
bank-mirror routing, MMX cooperative-scheduler HLE, abs-indirect
dispatch emit, dispatch-site m/x tracking) that are now baked in.

## What's in this repo

- `recompiler/` — Python code that decodes 65816 ROM bytes,
  reconstructs control flow, and emits C.
- `runner/` — C runtime that the generated code links against (CPU
  state, memory mapping, debug server). Embeds a snes9x-derived
  oracle for differential testing.
- `tests/` — framework tests (decoder, CFG, SSA placement, etc.) and
  L3 fixtures.
- `fuzz/` — differential fuzzer that diffs synthetic 65816 snippets
  through the recompiler vs. an embedded snes9x.
- `tools/` — scripts for regen, oracle diffing, etc.

## Public API / docs

There isn't a public API yet, and there aren't user-facing docs.
Internal docs assume context from active development sessions and
will not make sense without it. This will change once the framework
stabilizes.

## License

Not yet declared. Code in this repo is original; the snes9x core
under `runner/snes9x-core/` is upstream from
[libretro/snes9x](https://github.com/libretro/snes9x) and retains
its own license.
