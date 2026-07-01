# SNES Differential Co-Simulation — design + gates

Full-architectural-state, first-divergence decision procedure for the snesrecomp
ecosystem. Method (agnostic) = `F:\Projects\recomp-template\DIFFERENTIAL-COSIMULATION.md`;
SNES instantiation = `.../SNES/DIFFERENTIAL-COSIM-PROPOSAL.md`; proven PSX reference impl
= `F:\Projects\psxrecomp\_wt-tomba2\psxrecomp\{COSIM_ORACLE.md,cosim.c,cosim_state.c,tools/cosim.py}`.

**Goal = fix audio, but start from the EARLIEST divergence in ANY subsystem.** The tool
finds whatever splits first, wherever. The `SNES_ACCURACY_BURNDOWN.md` "off-cue not
off-tone / synthetic clock" story is a HYPOTHESIS this tool GRADES — not a fact we build
around. Hash the FULL state (incl. every S-DSP internal) so the per-subsystem sub-hash
tells us which subsystem stayed faithful and which split. No hypothesis-driven trimming.

Worktree: `F:\Projects\snesrecomp\_cosim_snesrecomp` [feat/snes-cosim] off engine `main`.
All code `#ifdef SNES_COSIM`; zero effect on normal builds.

## Key SNES facts that shape the build (verified in-tree 2026-07-01)

- The runner recompiles ONLY the 65816. The APU (SPC700+S-DSP), PPU, DMA, cart are the
  shared LakeSnes-lineage interp in `runner/src/snes/` (`apu.c dsp.c spc.c ppu.c dma.c
  cart.c snes.c`). So A and B share the *device code* by construction — the co-sim
  isolates the **recompiled 65816 + its APU pacing** against a faithful 65816 driving the
  same devices. (Device/DSP *synthesis* fidelity is Track B's job, vs bsnes.)
- Recomp CPU state = `CpuState g_cpu` (`cpu_state.h`): `A X Y S D DB PB P`, m/x/emul flags,
  flag-bit mirrors, `*ram`→`g_ram[128K]`, `cycles` (Axis-2 bus cycles, validated vs bsnes),
  `master_cycles` (Axis-5 region-weighted 21.47727 MHz master clocks). **No live PC field**
  (native control flow) — PC currency is the PSX caveat; exclude from the compared hash,
  report a last-leader PC for labels.
- `master_cycles` + `cycles` are ALREADY accumulated per-block by the CURRENT generated C
  (Axis-2 shipped; "nothing reads it yet"). So the alignment clock is live WITHOUT regen.
- Full-state serializers already exist: `snes_saveload` → `cpu/apu/dma/ppu/cart_saveload`;
  `apu_saveload` → SPC RAM (64K) + `dsp_saveload` (whole `Dsp` blob: channels/ADSR/BRR/echo
  FIR/noise) + `spc_saveload` (SPC700 regs). This IS the "hash the full DSP internals"
  surface, essentially free — repurpose the `SaveLoadInfo` callback to fold into a hash.
- APU catch-up: `rtl_accumulate_apu_catchup` in `common_rtl.c` converts a master-cycle
  delta → SPC cycles; the synthetic `+256/APU-touch` estimate (`cpu_state.c
  cpu_pace_cycles`) is the historical off-cue root. Both are hashed as state.

## The two tracks (user decision 2026-07-01 — both wanted)

**Track A — SMW, in-project reference (FIRST; validates the harness).**
- A = SMW recomp runtime, `snes-cosim` build.
- B = a headless standalone build (`snes-cosim-ref`, its OWN globals = separate process)
  using **`interp816.c`** as the 65816 (the live in-runner LakeSnes interpreter —
  `interp816_runOpcode` + caller-supplied bus) driving the runner's OWN `runner/src/snes/`
  devices (apu/dsp/spc/ppu/dma/cart/snes). NOTE: `runner/src/snes/cpu.c`'s interpreter was
  ripped 2026-04-20 (0 opcode cases — now just a register holder), so interp816 IS the
  reference CPU. NOT the separately-drifted `SuperMarioWorldRecomp-oracle` copy (its cpu.c
  is a full interp but the device layouts may have drifted). Same source ⇒ **identical
  device struct layouts** (raw saveload-hash compares directly) AND **shared
  `snes_cycles.h`** (so `master_cycles` aligns as a cycle-accurate ruler). Still an
  in-project DSP/APU reference.
- Fixture: SMW attract/demo (no input → identical inputs by construction).

**Track B — MMX, bsnes external oracle (STAGE AFTER A; loudest audio payoff).**
- A = MMX recomp runtime (same `snes-cosim` harness — MMX and SMW share the runner).
- B = bsnes via a libretro frontend (`snesref/frontend.cpp` is a generic frontend; swap
  `snes9x_libretro.dll` → `bsnes_libretro.dll`). CAVEAT: stock libretro is FRAME-granular
  with an opaque savestate blob → cycle-granular stepping + S-DSP-internal introspection
  likely needs patching bsnes (`F:\Projects\_bsnes_src`), or accept frame-granular
  checkpoints. Ruler here = bsnes master cycles (`bsnes_total_guest_cycles()`).

## The shared alignment clock (requirement 3 — load-bearing)

**Track A ruler = the guest MASTER CLOCK** (`g_cpu.master_cycles` ↔ interp master cycle).
Valid because B is the runner's own interp sharing `snes_cycles.h` → both advance it
identically as a pure function of guest execution, up to the true divergence. It is also
literally the audio-timing clock, so the APU-pacing divergence shows against it cleanly.

- **Fallback ruler = retired 65816 instruction count** (model-INDEPENDENT). Kept in reserve
  for Track B / any reference with a different cycle model; both sides advance it identically
  regardless of cycle weighting. (Recomp would emit per-block static instr-count at leaders.)
- `cycles`/`master_cycles` are HASHED-AS-STATE only in a reported "clk" bucket, NOT in the
  compared hash — a cycle-model diff surfaces as a reported field diff, never a false halt
  (both sides are AT the same master-cycle checkpoint by construction, so equal there anyway).

Checkpoint on strides of the ruler. **Park at every stride boundary; advance only on
`step N`** (launch-fixed stride via env, before either process runs an instruction — no
set-stride race, no async-stop park skew; the two PSX harness bugs). Drill finer by
shrinking the stride and re-running (deterministic ⇒ same divergence reproduces).

## Full state to hash (WHOLE machine — no hypothesis trimming)

ONE shared `cosim_state.c` compiled into BOTH builds, so the hash is provably identical.
Per-subsystem sub-hashes (FNV-1a via a `HashSli` `SaveLoadInfo`):

- `cpu` — canonical field emit in FIXED order/width: A,X,Y,S,D (LE16), DB,PB,P (u8),
  emulation (u8). `#ifdef SNES_COSIM_REF` reads the `Interp816` (a,x,y,sp,dp,k,db + flags→P,
  e); else `g_cpu` (recomp: A,X,Y,S,D,DB,PB,P,emulation). Both emit the SAME canonical stream.
  EXCLUDE: PC (no live recomp PC — report last-leader only), `host_return_valid` +
  flag-bit mirrors (recomp C-ABI/derived micro-state, non-architectural).
- `ram` — `snes->ram` 128K (recomp `g_snes->ram` == `g_ram`).
- `apu` — `apu_saveload` (SPC RAM + DSP + SPC700). Also expose `dsp`/`spc` sub-hashes
  separately for finer localization.
- `ppu` `dma` `cart` — respective saveloads (Track A determinism needs PPU H/V; include it).
- `sio` — the `snes` blob `hPos..divideResult` (H/V pos, apuCatchupCycles, IRQ/NMI/timers,
  joypad, mult/div) + `ramAdr`.
- `pace` — recomp-only: `g_apu_pace_cycles_estimate`, `g_apu_last_sync_master`,
  `g_main_cpu_cycles_estimate`, `g_memsel`, the 4 APU I/O port latches. (Provenance for the
  synthetic clock IF it's the producer — included as state, not assumed culprit.)
- `clk` (REPORTED, not compared) — `cycles`, `master_cycles`.

Do NOT call `snes_saveload` wholesale on the recomp side (its `cpu_saveload(snes->cpu)`
hashes the DORMANT interp CPU = stale). Call device saveloads individually + `cosim_hash_cpu`
for the live CPU. Same on the ref side (skip the device-pass CPU; use `cosim_hash_cpu`) so
both builds hash identically.

Chain hash = running fold of (cp, ruler, all compared sub-hashes) so any past divergence
sticks. Gate-4 fault injection (`inject ram|reg`) built into the state module.

## Validation gates — trust NOTHING until all pass

1. **A-vs-A = 0** across the attract run (recomp-vs-recomp; force headless, single-thread,
   NO host audio sink / SDL / resampler — likely nondeterminism suspects).
2. **B-vs-B = 0** (interp-vs-interp).
3. **Injected fault halts at the right cp + names the subsystem** (flip 1 WRAM byte / 1 APU
   port after cp K → must halt ~K, only that sub-hash differs). This is the ONLY gate that
   catches a silently-blind coordinator (`None==None`); never skip it. Assert compared
   fields parsed non-null.
4. **Hash-vs-byte audit** every N cp (force full byte compare even when hashes match).

Only after 1–4: run A-vs-B on attract → read which sub-hash splits first → bracket →
field-diff → NAME → faithful fix → rebuild → re-run. Acceptance = audio cue lands on time
by ear, not "hashes match".

## Production discipline (STANDING CONSTRAINT — user, 2026-07-01)

The co-sim and the interp are DIAGNOSTICS, not production infrastructure.

1. **Co-sim is dev-build only.** The entire co-sim (`cosim_state.c`, `cosim.c`, the ref
   driver, the TCP server, all `#ifdef SNES_COSIM` code) compiles ONLY in a dedicated
   dev/diagnostics target. It is NEVER in the shipping Production config — zero bytes in
   released exes. (Matches the standing "validators are dev-only" rule.)

2. **`interp816` must NOT be silently load-bearing in production.** It is the interp-
   fallback tier; if a shipped build ever tier-downs into it (a coverage gap slipped
   through), that MUST be LOUD — impossible to miss in a windowed build. Today's signal
   (`interp_bridge.c interp_tier_note`) is first-32-hits→`stderr` + an in-memory manifest:
   adequate for a dev console, but **effectively silent in a shipped windowed exe** (no
   console, capped at 32, no on-disk/on-screen trace). Follow-up (separate from the co-sim):
   a production-visible signal that works without a console — persistent log-file line
   (uncapped, distinct-site deduped) + a one-time on-screen banner on first prod tier-down.

## Build shape

- `runner/src/cosim_state.{c,h}` — the shared full-state hash (this task).
- `runner/src/cosim.c` — park/step engine + minimal TCP server; poll hooked into the
  runtime memory-access helpers (`cpu_read/write` in `cpu_state.c`) reading
  `g_cpu.master_cycles` — no regen needed for v1.
- `snes-cosim` game target (heavy diagnostics OFF, headless, single-thread) gated on a
  CMake/MSBuild option; `#ifdef SNES_COSIM`.
- `snes-cosim-ref` headless driver linking `runner/src/snes/*.c` (Track A B-side).
- `tools/snes_cosim.py` — coordinator (launch both, stride via env, step-compare, first-
  divergence + sub/field/window report; drives the gates).

## Protocol (line-oriented TCP, `#ifdef SNES_COSIM`)

```
status                 cp, ruler(master_cycles), parked flag
step N                 run N checkpoints, park, reply: cp, ruler, chain-hash
chain                  cumulative chain hash + cp + ruler
sub                    per-subsystem hashes of current state
cpu                    full CPU field dump (field-diff)
dev                    device-timing field dump (H/V, IRQ, timers, ports, pace)
window N               last N checkpoint rows (bounded reporting)
inject ram A V | reg R V   gate-3 fault injection
reset                  reset incremental/chain hash state
```
