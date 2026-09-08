<p align="center">
  <img src="docs/assets/snesrecomp-logo.png" alt="SNESRecomp" width="640">
</p>

# SNESRecomp

**A general-purpose static recompiler for the Super Nintendo Entertainment
System (Super Famicom).** SNESRecomp translates 65816 machine code into C,
compiles it into a native executable, and links it against a shared SNES
hardware runtime.

The game CPU runs as native code instead of inside a full-system emulator.
The runtime models the hardware around it—including the PPU, APU, DSP, DMA,
cartridge mapping, and supported enhancement chips—and provides a safe
interpreter tier for code that cannot yet be resolved statically.

Projects built on SNESRecomp already support true widescreen views, Adaptive
View, versioned mod packages, MSU-1 audio, cross-platform builds, launchers,
and save states. The framework is game-agnostic: each title supplies its own
analysis configuration and integration code while improvements to the CPU and
hardware model benefit every project.

<table>
  <tr>
    <td width="29%"><img src="docs/assets/games/super-mario-world-wide.png" alt="Super Mario World running in SNESRecomp at 16:9" width="100%"><br><sub><b>Super Mario World</b></sub></td>
    <td width="41%"><img src="docs/assets/games/alttp-adaptive.png" alt="The Legend of Zelda: A Link to the Past running in SNESRecomp with Adaptive View" width="100%"><br><sub><b>The Legend of Zelda: A Link to the Past</b></sub></td>
    <td width="30%"><img src="docs/assets/games/mega-man-x-wide.png" alt="Mega Man X running in SNESRecomp at 16:9" width="100%"><br><sub><b>Mega Man X</b></sub></td>
  </tr>
</table>

## Games

Each public game is maintained in its own repository, pins a known framework
revision, and ships its own ROM-free release. These projects are still alpha;
see each repository for its supported ROM revision, controls, build
instructions, and current validation status.

| Game | Repository | Latest build | Current status |
|---|---|---|---|
| *Super Mario World* | [SuperMarioWorldRecomp](https://github.com/mstan/SuperMarioWorldRecomp) | [releases](https://github.com/mstan/SuperMarioWorldRecomp/releases/latest) | Believed playable end to end; 4:3, fixed 16:9, Adaptive View, and MSU-1 audio. |
| *The Legend of Zelda: A Link to the Past* | [ZeldaAlttPSNESRecomp](https://github.com/mstan/ZeldaAlttPSNESRecomp) | [releases](https://github.com/mstan/ZeldaAlttPSNESRecomp/releases/latest) | Playable through the early dungeon; Adaptive View and MSU-1 audio. |
| *Mega Man X* | [MegaManXSNESRecomp](https://github.com/mstan/MegaManXSNESRecomp) | [releases](https://github.com/mstan/MegaManXSNESRecomp/releases/latest) | Fully playable; experimental true-widescreen mod; Windows, macOS, and Linux builds. |

## ROM compatibility

SNESRecomp is not a drop-in emulator: support in this table means that the
analyzer and shared runner understand the cartridge mapping or enhancement
chip. Each game still needs its own analysis configuration, integration, and
validation before it becomes a playable native port. The companion game
repositories are authoritative for supported ROM regions and revisions.

| Cartridge mapping or chip | Status | Notes |
|---|---|---|
| Standard LoROM and FastROM | **Supported** | ROM and battery-backed SRAM mapping are implemented. Headerless `.sfc` and copier-headered `.smc` images are accepted. |
| Standard HiROM and FastROM | **Supported** | ROM and battery-backed SRAM mapping are implemented and detected automatically. |
| Capcom Cx4 / CX4 | **Supported** | Instruction-level support for the chip used by *Mega Man X2* and *Mega Man X3*. Its internal data ROM is synthesized and does not require a separate firmware file. See [`runner/src/snes/CX4_NOTES.md`](runner/src/snes/CX4_NOTES.md). |
| Nintendo Super FX / GSU | **Supported** | Instruction-level core and cartridge mapping are implemented. *Star Fox* is the current development validation target; other Super FX games have not yet been qualified individually. |
| Nintendo DSP-1 / DSP-1B | **Supported** | Supported game: Super Mario Kart. Its canonical SHVC-1K1X windows, an instruction-level NEC uPD7725 core, and a firmware-free command model for the firmware-verified command set are implemented. Supplied `dsp1b.rom`, `dsp1.rom`, and word-reversed `dsp1.bin` images always select LLE; when none is found, HLE is used and stops loudly on an unverified command. Both backends pass unit and differential command gates, a deterministic one-player race matches Snes9x WRAM/DSP behavior and framebuffer output within renderer rounding, and the canonical 36,000-frame LLE/HLE attract qualifications pass. A title-gated Fast HiROM mapping supports converted development derivatives without broadening detection to other NEC DSP boards. |
| Nintendo SA-1 | **Supported** | Supported game: Super Mario RPG. Instruction-level 65816 execution, ROM-bus arbitration, Super MMC banking, IRAM/BW-RAM and bitmap windows, CPU/SA-1 interrupt communication, timers, DMA/character conversion, arithmetic, and variable-bit reading are implemented. The focused hardware suite and the canonical US 18,000-frame attract-loop qualification pass without runtime, logic-activity, video-activity, or audio-activity failures. See [`runner/src/snes/SA1_NOTES.md`](runner/src/snes/SA1_NOTES.md). |
| MSU-1 | **Supported, opt-in** | The extension's registers, data channel, and PCM audio are implemented, but a game must integrate an MSU-1 driver and pack selection. See [`docs/MSU1.md`](docs/MSU1.md). |
| ExLoROM, ExHiROM, and other custom mappings | **Not supported yet** | The current mapper layer handles standard LoROM, HiROM, SA-1, Cx4, and Super FX layouts only. |
| Nintendo DSP-2, DSP-3, and DSP-4 | **Not supported yet** | These cartridge DSP firmwares use the same NEC family interface but need per-title board/firmware validation before being advertised. |
| S-DD1 | **Experimental** | Cartridge register/MMC mapping and decompression hooks are present for Star Ocean validation. Treat as title-gated until provenance, savestate, and cross-title behavior are reviewed. |
| SPC7110 and SPC7110 RTC | **Not supported yet** | No data-decompression, mapping, or RTC model is present. |
| OBC-1, ST010, ST011, ST018, and S-RTC | **Not supported yet** | Their register windows and coprocessor behavior are not modeled. |
| BS-X, Sufami Turbo, and Super Game Boy cartridge adapters | **Not supported yet** | Their special cartridge or subsystem behavior is outside the current mapper model. |

Chips and mapping schemes not listed as supported should be assumed
unsupported. Some unsupported cartridges may get far enough through header
detection to load as a plain LoROM or HiROM image, but their unmodeled register
windows will still prevent correct execution.

## Development showcase

<table>
  <tr>
    <td width="50%"><img src="docs/assets/games/mega-man-x2-wide.png" alt="SNESRecomp development screenshot" width="100%"></td>
    <td width="50%"><img src="docs/assets/games/mega-man-x3-wide.png" alt="SNESRecomp development screenshot" width="100%"></td>
  </tr>
  <tr>
    <td><img src="docs/assets/games/star-fox.png" alt="SNESRecomp development screenshot" width="100%"></td>
    <td><img src="docs/assets/games/super-metroid.png" alt="SNESRecomp development screenshot" width="100%"></td>
  </tr>
</table>

## What it is

SNESRecomp turns a ROM into a recompilation project:

1. The analyzer discovers code, follows control flow, tracks the 65816 M/X
   width state, and resolves direct and indirect dispatch where it can.
2. The emitter translates discovered 65816 functions into portable C.
3. A normal C compiler builds that output together with the shared runner and
   game-specific integration code.
4. At runtime, unresolved or dynamically reached code can fall through to the
   interpreter tier instead of becoming a correctness hole.

This is a hybrid low-level design. The original game logic remains the source
of truth: recompiled CPU code drives modeled SNES hardware at the same
register boundaries a cartridge uses. Game projects can add narrowly scoped
high-level helpers or presentation enhancements, but the low-level path stays
available for validation.

SNESRecomp is a **framework**, not a collection of ROMs. It does not include
copyrighted game data, and a generated project is only the starting point for
a playable port. A new title still needs accurate function boundaries,
indirect-dispatch configuration, validation, and a host application.

## Widescreen and Adaptive View

Display aspect and view width are separate controls. The shared desktop policy
supports **4:3 (CRT)** with the SNES's 7:6 horizontal pixel correction,
**8:7 (Square pixels)** for a 1:1 pixel aspect, and a literal **1:1 (Square
frame)** presentation. Games opt into the launcher control and persist it as
`DisplayAspect`; 4:3 remains the default.

These are genuinely wider views—not stretched 4:3 output. SNES 2D engines
normally stream backgrounds, spawn objects, and cull sprites for a 256-pixel
viewport. A correct widescreen integration widens those systems together while
leaving game simulation and authentic 4:3 behavior unchanged.

A fixed widescreen view adds one third more horizontal logical pixels. Because
the chosen pixel shape stays unchanged, that expansion presents as 16:9 from
4:3, 32:21 from 8:7, or 4:3 from a square frame.

<table>
  <tr><td><img src="docs/assets/games/super-mario-world-wide.png" alt="Super Mario World true widescreen at 16:9" width="100%"></td></tr>
  <tr><td align="center"><sub><b>Super Mario World — fixed 16:9.</b> The native 224-pixel logical height is retained while the level view expands horizontally.</sub></td></tr>
</table>

<table>
  <tr><td><img src="docs/assets/games/mega-man-x-wide.png" alt="Mega Man X true widescreen at 16:9" width="100%"></td></tr>
  <tr><td align="center"><sub><b>Mega Man X — true widescreen.</b> Background streaming, object windows, HUD placement, and sprite rendering are widened together.</sub></td></tr>
</table>

Adaptive View grows or clamps the logical viewport according to the available
scene and window width. At a map boundary it can preserve an authentic edge
instead of wrapping or inventing scenery.

<table>
  <tr>
    <td width="32%"><img src="docs/assets/games/alttp-standard.png" alt="A Link to the Past at its authentic 4:3 view" width="100%"></td>
    <td width="68%"><img src="docs/assets/games/alttp-adaptive.png" alt="A Link to the Past with Adaptive View" width="100%"></td>
  </tr>
  <tr>
    <td align="center"><sub><b>Authentic 4:3</b></sub></td>
    <td align="center"><sub><b>Adaptive View</b></sub></td>
  </tr>
</table>

The transferable 2D-engine checklist lives in
[`docs/WIDESCREEN_PATTERNS.md`](docs/WIDESCREEN_PATTERNS.md).

## Mods

SNESRecomp supports opt-in, versioned `.snesmod` packages. A package is an
installation, update, provenance, and trust boundary; it may expose multiple
independently toggleable features and typed options in the launcher.

Packages contain data only—never DLLs or arbitrary native code. Native behavior
is owned by the game, statically linked into its executable, and registered
under stable plugin IDs. Before launch, the runtime verifies the selected stock
ROM, resolves enabled features, rejects missing or conflicting plugins, and
then activates the trusted game-side implementations.

The system is off by default at the framework level, so a game must explicitly
enable and integrate it. Mega Man X uses this path for its built-in
true-widescreen feature. See
[`docs/MOD_PACKAGES.md`](docs/MOD_PACKAGES.md) for the package format and trust
model.

Runtime localization uses the same data-only mod boundary for language-gated
ROM, RAM, and VRAM patches. See
[`docs/RUNTIME_LOCALIZATION.md`](docs/RUNTIME_LOCALIZATION.md).

## MSU-1 audio

The shared runner implements the MSU-1 registers, data channel, and 44.1 kHz
stereo PCM streaming mixed over the S-DSP output. SMW and ALttP integrate this
with their launchers: select a compatible music-pack folder and keep using a
verified stock ROM.

Their MSU-1 drivers are compiled into the executable from a temporary,
locally-patched analysis image during regeneration. The user's stock ROM is
not modified. With no pack selected, the games retain their authentic SPC
soundtracks.

The chip is inert by default for projects that do not integrate it. Developers
can also point `SNESRECOMP_MSU1` at a pack directory or base prefix directly.
See [`docs/MSU1.md`](docs/MSU1.md) for the register model, pack resolution, and
game-integration details.

## How to use SNESRecomp

### Generate a project with the released CLI

1. Download `snesrecomp-cli-windows-x86_64.zip` from
   [Releases](https://github.com/mstan/snesrecomp/releases).
2. Extract the entire archive to one folder.
3. Open PowerShell there and run:

```powershell
.\snesrecomp.exe build `
  --rom "C:\Games\My Game\game.sfc" `
  --output "C:\Projects\MyGameRecomp"
```

Both `.sfc` and `.smc` images are accepted. Standard LoROM and HiROM mapping
are detected automatically.

The generated folder contains discovered C source, a starter bank
configuration, CMake build files, and the runner sources needed for further
integration. The released CLI is self-contained; generating a project does
not require Python, Rust, or a source checkout.

### Build the generated source

Install CMake, Ninja, and a C compiler, then run:

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Projects\MyGameRecomp\build.ps1"
```

The generated project also includes `build.sh` for macOS and Linux. This step
builds a static library containing the generated code; turning an arbitrary
game into a playable native port still requires the game-specific work
described above.

Use only a ROM image you obtained legally. SNESRecomp neither includes ROM
data nor copies the input ROM into the generated project. Generated C is
derived from that ROM, so do not redistribute it without permission.

### Build the CLI from source

You need Git, Python 3.9 or newer, Rust 1.85 or newer, and PyInstaller:

```sh
git clone https://github.com/mstan/snesrecomp.git
cd snesrecomp
python -m pip install pyinstaller==6.21.0
python tools/build_cli.py release
```

The ready-to-use ZIP is written to `dist/`.

## Choosing SDL3 or SDL2 for a desktop game

SDL3 is the default desktop backend for CMake game hosts. SDL2 remains a
supported compatibility fallback, but it must be selected explicitly. The
choice applies to both the game window and the shared recomp-ui launcher, so a
build never mixes SDL major versions.

Install the development package for the backend you want:

- Windows: use an SDL development package for your compiler and architecture.
  For the official MinGW archives, point CMake at the contained
  `x86_64-w64-mingw32` directory.
- macOS: install `sdl3` for the default build or `sdl2` for the fallback.
- Linux: install your distribution's SDL3 development package, or its SDL2
  development package for the fallback.

Keep separate build directories for the two backends. This avoids stale CMake
package paths and also makes side-by-side testing straightforward.

### Build with SDL3 (default)

From a game repository that consumes SNESRecomp:

```powershell
$engine = 'C:\src\snesrecomp'
$ui = 'C:\src\recomp-ui'
$sdl3 = 'C:\deps\SDL3\x86_64-w64-mingw32'

cmake -S . -B build-sdl3 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DSNESRECOMP_ROOT="$engine" `
  -DRECOMP_UI_ROOT="$ui" `
  -DCMAKE_PREFIX_PATH="$sdl3"
cmake --build build-sdl3 --parallel
```

`-DSNESRECOMP_SDL_BACKEND=SDL3` is accepted but optional because SDL3 is the
default.

### Build with the SDL2 fallback

Configure a different build directory and select SDL2 explicitly:

```powershell
$engine = 'C:\src\snesrecomp'
$ui = 'C:\src\recomp-ui'
$sdl2 = 'C:\deps\SDL2\x86_64-w64-mingw32'

cmake -S . -B build-sdl2 -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DSNESRECOMP_ROOT="$engine" `
  -DRECOMP_UI_ROOT="$ui" `
  -DSNESRECOMP_SDL_BACKEND=SDL2 `
  -DCMAKE_PREFIX_PATH="$sdl2"
cmake --build build-sdl2 --parallel
```

If SDL is installed in a standard system prefix, omit
`-DCMAKE_PREFIX_PATH`. On Windows, the build helper copies the selected
`SDL3.dll` or `SDL2.dll` beside each executable.

During configuration, verify that CMake prints matching lines:

```text
<game-target>: SDL3 desktop backend
recomp-ui: SDL3 platform backend
```

The SDL2 build reports `SDL2 compatibility backend` instead. After a clean
game exit, `last_run_report.json` records the compiled and loaded SDL versions.

### Add backend selection to a game project

A game CMake file should use the shared helper instead of calling
`find_package(SDL2)` or `find_package(SDL3)` itself:

```cmake
set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to SNESRecomp")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/recomp-ui" CACHE PATH
    "Path to recomp-ui")
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)

add_executable(my_game ${SNESRECOMP_RUNNER_SOURCES} ${GAME_SOURCES})
target_include_directories(my_game PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
snesrecomp_target_sdl(my_game)
recomp_target_launcher_ui(my_game)
```

`snesrecomp_target_sdl()` owns package discovery, compile definitions, linking,
and Windows runtime-DLL staging. See
[`docs/SDL_BACKENDS.md`](docs/SDL_BACKENDS.md) for the benchmark mode and
maintainer notes. Some legacy Visual Studio game projects deliberately remain
on the explicitly defined SDL2 fallback; the CMake release path is the SDL3
default.

## Architecture

| Path | Purpose |
|---|---|
| `recompiler/` | Python reference analyzer and authoritative C emitter. |
| `recompiler-rs/` | Production native whole-program analyzer; preserves the Python manifest/emitter boundary while accelerating analysis. |
| `runner/` | Shared CPU state, memory map, hardware model, interpreter tier, launcher helpers, diagnostics, and enhancement hooks. |
| `tests/` | Analyzer, decoder, interpreter, runtime-dispatch, launcher, and PPU tests. |
| `fuzz/` | Differential fuzzing over synthetic 65816 programs. |
| `tools/` | Generation, validation, trace-diff, and release tooling, including the `snesref` reference frontend. |

Useful technical references:

- [`docs/FRAME_MODEL_HOSTS.md`](docs/FRAME_MODEL_HOSTS.md) - integration notes
  for hosts that drive their own frame or beam loop.

- [`docs/MULTI_TIER.md`](docs/MULTI_TIER.md) — static and interpreted execution
  tiers.
- [`docs/LLE_FIRST_ANALYSIS.md`](docs/LLE_FIRST_ANALYSIS.md) — analysis policy
  and the low-level correctness floor.
- [`docs/TRIPWIRES.md`](docs/TRIPWIRES.md) — runtime checks for M/X state,
  dispatch, and control-flow failures.
- [`docs/LAUNCHER_DESIGN.md`](docs/LAUNCHER_DESIGN.md) — shared launcher
  integration.
- [`docs/HOST_OVERLAY_EXTRACTION.md`](docs/HOST_OVERLAY_EXTRACTION.md) —
  extracting PPU layers for host-side composition.

## Reference debugging

SNESRecomp has two complementary reference paths. Both are developer tools and
are excluded from release packages and normal game builds.

| Tool | Reference boundary | Best use |
|---|---|---|
| [`tools/snesref/`](tools/snesref/) | Loads an independently developed bsnes, Snes9x, or other libretro core at runtime. | Validate framebuffer output, audio, inputs, frame timing, and memory behavior without sharing SNESRecomp's device implementation. |
| [`cosim/ref_driver.c`](cosim/ref_driver.c) + [`tools/snes_cosim.py`](tools/snes_cosim.py) | Runs `interp816` in a separate process against the same runner devices and exposes the same `SNES_COSIM_PORT` TCP protocol as a recompiled co-sim target. | Find the first CPU/runtime divergence with full-state and per-subsystem hashes. |

Use `snesref` when the symptom could be in a shared PPU, APU, cartridge, or
coprocessor model. Use the TCP co-simulation path to localize a difference
between recompiled 65816 execution and interpreted execution. A clean internal
co-sim is not proof that rendering or audio matches hardware because both sides
intentionally share those device implementations.

### Independent libretro reference

Build the small SDL2 frontend, then provide an emulator core DLL and a legally
obtained ROM at runtime:

```powershell
cd tools\snesref
.\build.bat

$env:SNESREF_FAST = "1"
$env:SNESREF_FRAMES = "1800"
$env:SNESREF_TRACE_FILE = "wram.jsonl"
$env:SNESREF_WAV = "reference.wav"
.\snesref.exe C:\cores\bsnes_libretro.dll C:\roms\game.sfc
```

For reproducible gameplay, set `SNESREF_INPUT_FILE` to a text file containing
`start-frame:duration:hex-mask`, one event per line. The mask layout is
`B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R` from bit 0 through bit 11. Frame
dumps are controlled by `SNESREF_FRAME_DUMP_DIR` and the optional
`SNESREF_FRAME_DUMP_FROM`, `_TO`, and `_STEP` variables. See
[`tools/snesref/README.md`](tools/snesref/README.md) for the complete capture
interface.

The frontend source and MIT-licensed `libretro.h` are tracked. Emulator cores,
SDL binaries, ROMs, firmware, captures, and `snesref.exe` are not tracked or
included in releases. In particular, bsnes is GPLv3 and Snes9x has a
non-commercial license; developers supply either one separately.

### Internal TCP co-simulation

The reference-only target does not need a game checkout:

```powershell
cmake -S cosim -B build\cosim-ref -G Ninja `
  -DSNES_COSIM_REF_ONLY=ON `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe
cmake --build build\cosim-ref --target smw_cosim_ref

.\build\cosim-ref\smw_cosim_ref.exe C:\roms\game.sfc `
  --frames 1800 --final-frame-dump reference.raw
```

Despite its historical target name, `smw_cosim_ref` is the game-neutral
interpreter side. For lockstep comparison, build the game-specific A-side
co-sim target and let the coordinator launch both processes:

```powershell
python tools\snes_cosim.py `
  --a-cmd "build/cosim/smw_cosim.exe C:/roms/smw.sfc" `
  --b-cmd "build/cosim/smw_cosim_ref.exe C:/roms/smw.sfc" `
  --stride 1 --max 3600
```

The coordinator injects separate `SNES_COSIM_PORT` values, advances both
servers at deterministic checkpoints, and stops at the first full-state
divergence. Run the A-vs-A, B-vs-B, fault-injection, and hash-audit gates in
[`SNES_COSIM.md`](SNES_COSIM.md) before trusting an A-vs-B result.

## Status

SNESRecomp is alpha software. Multiple games run through the same shared
framework, but APIs, generated-code conventions, and internal integration
points can still change. Per-game maturity varies; the [Games](#games) table
and each project's release notes are the authoritative status sources.

The current direction is to move recurring fixes into the framework, retain
an exact low-level fallback for uncertain code, and keep authentic output as
the regression baseline for every optional enhancement.

## Contributing

Bug reports and framework improvements are welcome. For a game-specific issue,
open it in that game's repository and include the ROM revision, platform,
configuration, reproduction steps, and a screenshot or save state when useful.

When bringing up a new game, keep game-specific addresses and behavior in its
companion repository. Reusable CPU, mapper, coprocessor, diagnostics, launcher,
and presentation mechanisms belong here.

## Acknowledgements

SNESRecomp did not start from scratch. Its runtime and tooling stand on prior
reverse-engineering and emulation work:

- **[snesrev](https://github.com/snesrev)** (`snesrev/zelda3`,
  `snesrev/smw`) — the C runner and surrounding ecosystem were heavily based
  on the snesrev reverse-engineered ports. The model of recompiling or porting
  CPU code to C, modeling the surrounding silicon, and validating against a
  reference emulator comes from that work. Runtime utilities, ROM verification,
  function-boundary conventions, and the default input layout were also
  adapted from it.
- The C SNES hardware core under
  [`runner/src/snes/`](runner/src/snes/) derives from
  **[LakeSnes](https://github.com/elzo-d/LakeSnes)** by elzo-d, as vendored by
  snesrev, with individual algorithms credited inline to **snes9x**.
- **[IsoFrieze/SMWDisX](https://github.com/IsoFrieze/SMWDisX)** — the Super
  Mario World disassembly used as the symbol and RAM-map basis and as a
  conformance reference. SMWDisX in turn credits mikeyk's original 2013
  disassembly and loveemu's SPC700 work.

## License

SNESRecomp's original code is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). Third-party components retain
their own licenses as documented in [Acknowledgements](#acknowledgements) and
[`THIRD_PARTY_ATTRIBUTION.md`](THIRD_PARTY_ATTRIBUTION.md). The `snesref` tool
loads a separately supplied libretro emulator core at runtime; no emulator core
source or binary is vendored or released by this repository.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
