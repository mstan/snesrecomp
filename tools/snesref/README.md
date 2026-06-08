# snesref

A standalone, hardware-accurate **reference interpreter with debugging tools**,
used as the differential oracle when chasing bugs in the recompiled build.

> Formerly named `mmxref`, then `snes-oracle`; settled on **snesref** to pair
> with the Genesis equivalent [`mdref`](https://github.com/mstan/mdref). Also
> published as a standalone repo at
> [`mstan/snesref`](https://github.com/mstan/snesref); this copy is vendored
> in-tree for convenience.

Under the hood it is a minimal SDL2 [libretro](https://www.libretro.com/)
frontend that loads a real SNES emulator core (an interpreter such as
`snes9x_libretro.dll`) and drives it while capturing the same instrumentation
the recomp runner's `debug_server` exposes. You play the game on a known-good
interpreter, capture a trace, and diff it against the recompiled run to pinpoint
the first divergence — register, RAM byte, or frame.

It is the successor to the old in-process `runner/snes9x-core` oracle (now
removed): instead of statically linking a patched emulator into every game's
`Oracle|x64` build, you run this one small tool against any libretro SNES core.

> Stood up while debugging Mega Man X (the Rangda Bangda eye and the Spark
> Mandrill dash-jump turtle), so its default capture filenames are `mmx_*` — but
> it works with any SNES ROM and any libretro SNES core.

## Why a separate interpreter, not the recompiler

The recompiler translates 65816 to native C ahead of time; subtle timing/state
bugs only show up as a *divergence from real hardware*. You need a trusted,
cycle-faithful reference to diff against. An interpreting emulator core is that
reference. Keeping it as a separate tool (rather than embedded in the runner)
means the shipping game exes carry none of it, and the reference can be swapped
for any libretro SNES core.

## Features

- **Per-frame WRAM diff trace** → `mmx_trace.jsonl`, one record per frame of
  changed WRAM bytes, in the exact JSON shape as the recomp `debug_server`'s
  `wram_writes_at`. Drop it beside a recomp capture and diff frame-by-frame.
- **Save/load state** (`F2`/`F4`) so you can park at a hard-to-reach game state
  and re-run the suspicious window deterministically.
- **Recomp-matched input** — the keyboard map mirrors the recomp runner's
  keybinds, so the same inputs reproduce the same run on both sides.
- **Any core, any ROM** — the emulator core is `argv[1]`; nothing is hard-wired
  to a specific core or game.
- **Fresh-capture toggle** (`F5`) to clear the trace and start a clean window.
- **Tiny + dependency-light** — ~300 lines of C++, SDL2, and the libretro API
  header. No build coupling to the recompiler.

## Build

```bat
:: 1. extract the SDL2 VC dev package here as SDL2-2.30.9\   (libsdl.org)
:: 2. build
build.bat
```

Produces `snesref.exe`.

## Run

```bat
snesref.exe <core.dll> <rom.sfc>
:: e.g. snesref.exe snes9x_libretro.dll mmx.sfc
```

Place the libretro core DLL (and its `SDL2.dll`) next to the exe, or pass a path.

### Keys (match the recomp keybinds)

| Key | SNES | | Key | Action |
|-----|------|-|-----|--------|
| Arrows | D-pad | | F2 | save state → `mmx_state.bin` |
| Z | B (jump) | | F4 | load state |
| X | A | | F5 | clear `mmx_trace.jsonl` (fresh capture) |
| A | Y (fire) | | Enter | Start |
| S | X | | RShift | Select |
| C / V | L / R | | Esc | quit |

## Licensing

This tool ships **only its own source**: `frontend.cpp` (this project's
license) and `libretro.h` (**MIT**, RetroArch team); SDL2 is **zlib**. The
emulator core is a **runtime DLL you supply** — e.g. `snes9x_libretro.dll`,
whose snes9x license is non-commercial. No core source or binary is committed
here (see `.gitignore`), so this repo carries none of a core's licensing terms.
