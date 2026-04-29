# snesrecomp

A static recompiler for SNES (Super Famicom) games. Translates 65816
machine code into native C ahead-of-time, so the recompiled game runs
as a normal binary rather than under interpretation.

> ## ⚠️ Heavily Work-In-Progress
>
> This project is **early, unstable, and not yet usable as a general
> tool**. Internal debugging notes and design docs reference
> incomplete passes, half-built infrastructure, and known
> regressions. Branches are noisy. APIs change without warning.
> Tests are not green on every branch. Expect things to be broken.
>
> Treat anything you see here as a snapshot of in-progress work, not
> a release.

## Status

snesrecomp currently supports exactly one game as its driving test
case:

- **Super Mario World** — recompiled via the companion repo
  [mstan/SuperMarioWorldRecomp](https://github.com/mstan/SuperMarioWorldRecomp).
  See that repo for the runtime, per-game config, and what works /
  doesn't work in the recompiled SMW build.

The intent is for snesrecomp to be **game-agnostic** — adding a
second game (Mega Man X, Contra III, F-Zero, …) should cost hours of
per-game `.cfg` work, not months of framework patching. We aren't
there yet; SMW is currently exercising the framework, surfacing gaps,
and driving feature work.

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

## Status of public API / docs

There isn't a public API. There aren't user-facing docs. Internal
docs assume context from active development sessions and will not
make sense without it. This will change once the framework
stabilizes.

## License

Not yet declared. Code in this repo is original; the snes9x core
under `runner/snes9x-core/` is upstream from
[libretro/snes9x](https://github.com/libretro/snes9x) and retains
its own license.
