# Setting up a game project

How a new SNES title goes from a ROM on your drive to a repository that
builds, regenerates, packages, and publishes itself.

```sh
sh tools/new_project/setup_project.sh --rom ~/roms/game.sfc --dir ~/src
```

That is the whole happy path — on a terminal every other setting is asked,
with the probed ROM identity supplying the defaults. Flags are for scripting:
anything passed explicitly skips its question, and `--yes` (or a non-TTY run)
takes every default.

The rest of this document is what it did and what you do next.

---

## 1. Before you start

**Check the coprocessor.** A title using a chip this runner does not emulate
is a much larger project than one that does not:

```sh
python3 tools/new_project/probe_rom.py ~/roms/game.sfc
```

`coprocessor=none` is the easy case. `SA-1`, `CX4`, `DSP` and `SuperFX` have
implementations in `runner/src/snes/`; anything reported as `unknown` means
the cartridge type byte is not one this tool recognises, and you should find
out what it is before investing.

**Check the checksum.** `checksum_valid=false` means the image is modified,
over-dumped, or not a plain ROM. Generating from one produces divergence that
surfaces much later and reads like a framework bug.

## 2. What it asks

| Question | Default |
|---|---|
| Display name | the dump filename cleaned of `(Region)` tags, or the cartridge header when that reads better |
| Max players (1-8) | 2 |
| Multitap port | derived from players; only asked above 2 seats |
| Release zip / CI artifact prefix | slug of the name |
| Description, publisher, year | blank |
| Region | from the cartridge header |
| recomp-ui launcher submodule | no |
| Netplay | no — skipped for a 1-player title |
| Rollback | yes, when netplay is on |
| GitHub Actions workflow | yes |
| Generate now / build after | yes / yes |
| Create a GitHub repo with `gh` | **no** |

**Nothing assumes a GitHub repository exists.** Declining skips creation, the
remote, and the push, and you still get a complete local git repository. If
you accept, the repo is created *after* the scaffold is committed and pushed
once at the very end — pushing earlier leaves a second "initial" commit that
collides when the script is re-run.

## 3. What the scaffold does, in order

1. **Probe** the ROM: mapping, title, region, coprocessor, reset/NMI vectors,
   CRC32 and SHA-256.
2. **Lay out** the repo and `git init`.
3. **Fill templates** — CMake, README, host sources, regen and packaging
   scripts — with the probed identity.
4. **Seed analysis config**: `recomp/bank00.cfg` with `auto_vectors` and
   `tier_down_stubs`, plus a starter `recomp/symbols.toml` naming the reset
   and NMI vectors.
5. **Add submodules**: `snesrecomp` (which owns `lib/recomp-net` and
   `lib/retcomm-rbengine`), optionally `recomp-ui`.
6. **Record pins** to `framework_pins.txt`.
7. **CI workflow**, **commit**, optional **`gh repo create`**.
8. **Generate** and **build** if asked.
9. **Push**, once, at the end.

## 4. What you get, and what you do not

You get a repository that **builds a real executable** — framework, generated
C, and a working desktop host — plus a regeneration pipeline that verifies the
ROM, CI, packaging, and a README that states what it is.

You do **not** get a working port. The scaffold stops exactly where
game-specific work starts, and that boundary is `src/game_rtl.c`.

Two responsibilities always land on the game host, and neither can be guessed:

- **What "one frame" means for this title.** A real ROM's reset vector never
  returns — it enters a main loop that waits on vblank — so the host chooses
  the yield point. The template uses the general LLE-first shape
  (`interp_bridge_run_until_quiescent` plus a master-clock deadline), which is
  the right starting point but is not tuned to any particular game.
- **Delivering NMI/IRQ at the hardware edge.** Nothing in the framework pushes
  an interrupt frame for you.

For a sense of scale, `MetalWarriorsSNESRecomp` carries roughly 11k lines of
per-game RTL. That is the work; everything the scaffold does is the part that
should not have to be done by hand each time.

### The host contract

`src/host_contract.c` defines the symbols the runner deliberately leaves to
the host. The scaffold fills them in with the smallest honest implementation:

| Symbol | What it decides |
|---|---|
| `Die` | what a fatal framework error does |
| `RtlApuLock` / `RtlApuUnlock` | guest thread vs audio thread serialisation |
| `g_spc_player` | optional SPC upload interception (NULL = guest's own path) |
| `debug_on_block_enter`, `debug_on_wram_write_*` | trace hooks |
| `RtlDrawPpuFrame` (in `main.c`) | how a finished frame reaches the screen |

## 5. Regenerating

```sh
bash tools/regen.sh --rom /path/to/game.sfc
```

Verifies the ROM against the digests baked into the project, generates
`src/gen/*.c`, and re-syncs `recomp/funcs.h`. Re-run it after every change
under `recomp/`.

The ROM does not have to live in the repository — `SNESRECOMP_ROM` sets the
path once for a shell. Generated C is derived from copyrighted data: it is
gitignored, and every developer regenerates from their own copy.

`--no-verify` skips the digest check for a revision the project is not pinned
to. Expect the generated C to differ; the pin exists so that difference is a
decision rather than a surprise.

## 6. Growing the port

1. **Make it boot** — `src/game_rtl.c`.
2. **Name things** — add `[[func]]` entries to `recomp/symbols.toml`, re-run
   `tools/regen.sh`. `emit = true` promotes a function into ahead-of-time
   codegen; `false` keeps it interpreted. The block inside `bank00.cfg` is
   regenerated from the toml, so edit the toml.
3. **Resolve dispatch misses after every run, before anything else.** An
   unresolved indirect target is the reason a port diverges, and it is cheap
   to fix early and expensive to fix late.
4. **Never edit `src/gen/` by hand,** and never synthesise a result to get
   past uncovered code. Fix the config, the analyzer, or the runtime, and
   regenerate. An invented value turns a loud failure into a silent wrong one.

## 7. Multiplayer

`--players N` sets the seat count. Above two it configures a Super Multitap —
port 2 for 3-5 seats, both ports for 6-8. Seats beyond the second are driven
with `RtlSetPadState`; see [MULTITAP.md](MULTITAP.md).

`--netplay` links recomp-net (delay-sync). `--rollback` additionally links
retcomm-rbengine; delay-sync remains the runtime default and
`SNES_NET_MODE=rollback` opts in. See [RECOMP_NET.md](RECOMP_NET.md) and
[ROLLBACK.md](ROLLBACK.md).

Every peer must agree on the seat count and the tap configuration. Opening a
session wider than the ports can route is refused rather than degraded.

## 8. CI

The generated workflow builds the framework, runs its test suite, and
syntax-checks the host sources. It deliberately does **not** build the game
target, because that needs `src/gen/` and therefore a ROM, and no ROM belongs
in CI.

To make CI build the real target, publish the generated C to a private assets
repository and fetch it with a token — the pattern
`MetalWarriorsSNESRecomp` uses. That is a decision for the project owner, so
the template ships the honest version: a pipeline that is green because of
what it actually verified.

## 9. Releasing

```sh
scripts/package_release.sh
```

Stages the executable plus assets into `dist/<prefix>-<version>-<platform>.zip`
and **refuses to package if it finds ROM data**. Tagging `v*` runs the release
job in the generated workflow.

Bump `VERSION` for a release: Release builds stamp it as the lobby version, so
two netplay peers can prove they are the same build. Non-release builds stamp
`dev`.
