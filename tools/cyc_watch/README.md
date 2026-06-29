# cyc_watch — Axis-2 cycle-accuracy validation

Validation harnesses for the shared cycle cost model
(`recompiler/snes_cycles.py` → `runner/src/snes/snes_cycles.h`). See
`SNES_ACCURACY_BURNDOWN.md` Axis 2.

## `cyc_equiv.c` — cycle-equivalence cross-check (step B, first validation)

Runs the **shared cost model** against an INDEPENDENT implementation — the
vendored LakeSnes interpreter (`interp816`), which carries its own
`cyclesPerOpcode[]` table + inline modifier logic. "Reference shelf, not
self-agreement": two independently-sourced models, executed on directed 65816
sequences, must agree per opcode.

Build + run (Windows / mingw; the Bash sandbox blocks gcc, use PowerShell):

```powershell
$wt  = "F:\Projects\snesrecomp\_wt-accuracy"
$gcc = "C:\msys64\mingw64\bin\gcc.exe"
& $gcc -std=c99 -Wall -Wextra -I "$wt\runner\src\snes" `
    "$wt\tools\cyc_watch\cyc_equiv.c" "$wt\runner\src\snes\interp816.c" `
    -o "$wt\tools\cyc_watch\cyc_equiv.exe"
& "$wt\tools\cyc_watch\cyc_equiv.exe"
```

Exit 0 = all asserted-equivalence cases agree. Known LakeSnes deviations are
reported as `DIVERGE` lines (documented, non-fatal): in every one the
authority matches the 65816 datasheet and LakeSnes does not.

### Results (2026-06-27)

- **Base cycles:** 256/256 opcodes match LakeSnes' `cyclesPerOpcode[]`
  byte-for-byte (static check in `tests/test_snes_cycles.py`).
- **Modifiers (execution):** m=0 (+1/+2), x=0 (+1), D.l≠0 (+1), branch
  taken/not-taken, native RTI/COP — all agree.
- **4 documented divergences** (authority = datasheet, LakeSnes deviates):
  | case | authority | LakeSnes | LakeSnes behavior |
  |---|---|---|---|
  | LDA abs,X page-cross (read) | 5 | 4 | omits the read page-cross +1 |
  | STA abs,X page-cross (write) | 5 | 6 | adds a spurious write cross +1 (store is fixed) |
  | RTI in emulation (e=1) | 6 | 7 | applies the native +1 unconditionally |
  | COP in emulation (e=1) | 7 | 8 | applies the native +1 unconditionally |

  The RTI/COP cases agree in **native** mode (e=0), the normal SNES game
  state, so they do not affect real workloads. BRK ($00) is excluded: this
  snesrecomp adaptation repurposes BRK as the AOT-bridge trap, not a real
  interrupt.

These findings make `interp816` usable as a stepping reference engine, with
its cycle output corrected against the authority at the four documented sites.

## `cyc_ring.{c,h}` — always-on per-instruction cycle ring

Bounded circular buffer recording EVERY executed instruction
`{seq, pc24, opcode, cyc_auth, cyc_ref, master}` from boot (eviction keeps
memory flat). Probes QUERY a window after the fact — never arm-then-capture
(ring-buffer discipline). API: `cyc_ring_find_pc` (anchor lookup),
`cyc_ring_region` (sum over a seq window), and `cyc_ring_region_anchors` —
the **two-anchor REGION** that measures the cycle cost of one START->END pass
(offset cancels; `start_pc == end_pc` measures one loop iteration).

## `cyc_trace.c` — ring demonstrator + self-test

Steps interp816 over a controlled native-16-bit RMW loop, filling the ring
with the shared-authority count (computed from pre-state + runtime predicates:
D.l, read page-cross, branch taken/cross) AND interp816's native count, then
queries it. Asserts the REGION delta against the hand-computed datasheet value
(one iteration = 17 cyc; full 3-iter loop = 50 cyc) and that authority ==
reference over the whole trace (this path hits none of the 4 divergence
sites). Build:

```powershell
& $gcc -std=c99 -Wall -Wextra -I "$wt\runner\src\snes" -I "$wt\tools\cyc_watch" `
    "$wt\tools\cyc_watch\cyc_trace.c" "$wt\tools\cyc_watch\cyc_ring.c" `
    "$wt\runner\src\snes\interp816.c" -o "$wt\tools\cyc_watch\cyc_trace.exe"
```

**Scope:** flat-RAM bus, not a full SNES bus (no PPU/DMA/APU/MMIO). This
validates the ring + REGION mechanism and the authority's runtime-predicate
path on a known code path. Booting a real ROM to an anchor needs a SNES bus
around interp816 (separate component); for real-ROM cycle ground truth use the
bsnes hook below.

## bsnes ground-truth cycle hook (`bsnes_cycle_hook.patch` + `bsnes_cycles_probe.c`)

The external accuracy oracle: a monotonic guest master-clock counter added to
bsnes, exported as `bsnes_total_guest_cycles()` (analog of psx Beetle's
`beetle_total_guest_cycles`). This is what breaks the "both can be identically
wrong" trap — the homemade model is validated against an accuracy-grade
emulator, not just against interp816.

`bsnes_cycle_hook.patch` (dev-only; bsnes source lives OUTSIDE the recomp repo
at `F:\Projects\_bsnes_src`) adds, atop libretro/bsnes @ `591b7e1`:
- a `uint64_t g_bsnes_total_master_cycles` incremented by 2 in
  `SuperFamicom::CPU::stepOnce` (sfc/cpu/timing.cpp) — the single master-clock
  chokepoint (CPU + DMA/HDMA both step the CPU thread through it);
- `extern "C" __declspec(dllexport)` getters `bsnes_total_guest_cycles()` /
  `bsnes_reset_guest_cycles()` in target-libretro/libretro.cpp (+ reset on
  retro_reset), with `bsnes_*` added to the link.T version script;
- two build fixes for the modern toolchain (GCC 15.2): reformulated the
  `~0ull >> 64 - Precision` constexpr in nall (GCC-15 ICE), and `-D_GNU_SOURCE`
  for the bundled SameBoy `gb` core's `vasprintf`.

Apply + build (mingw):
```
git clone https://github.com/libretro/bsnes.git && cd bsnes
git checkout 591b7e13b6914beffaa01084e4c0b7a5d9cc0673
git apply /path/to/bsnes_cycle_hook.patch
make platform=win -j6        # -> bsnes_libretro.dll with the cycle exports
```

`bsnes_cycles_probe.c` validates the hook end-to-end (LoadLibrary + libretro
boot, headless): the counter advances **357368 master cyc/frame** — exactly
one NTSC frame (262 lines x 1364) — confirming a faithful master-clock count.
Build/run:
```powershell
& $gcc -O2 -I F:/Projects/_bsnes_src/bsnes/target-libretro `
    "$wt\tools\cyc_watch\bsnes_cycles_probe.c" -o "$wt\tools\cyc_watch\bsnes_cycles_probe.exe"
& "$wt\tools\cyc_watch\bsnes_cycles_probe.exe" F:\Projects\_bsnes_src\bsnes_libretro.dll <rom.sfc>
```

**Keep overclocking OFF** in the core config so every CPU cycle is counted.

## CLOSED LOOP — model validated against bsnes (2026-06-27)

The hook now also exports a **CPU (bus+internal) cycle counter**
(`bsnes_total_cpu_cycles()`, incremented per `CPU::idle/read/write`) — the same
unit the recomp/authority model emits (master clocks weight 6/8/12 and aren't
directly comparable) — plus a **two-anchor REGION latch**: `bsnes_set_cyc_anchor
(idx, pc24)` latches the CPU-cycle count the first time the CPU fetches an
instruction at each anchor PC (`CPU::main`), read via `bsnes_get_anchor_cpu_cycles
(idx)` / `bsnes_anchor_hit(idx)`. Region Δ = latch[1] − latch[0] (the reset
offset cancels).

`build_test_rom.py` emits a minimal LoROM with a KNOWN instruction stream (so
the authority's prediction is exact), bracketed by anchor PCs:
- `static`  — base + width + branch-taken loop; region [$8000,$8011) = **60** cyc.
- `dynamics`— D.l≠0 dp load + abs,X read page-cross; region [$800B,$8011) = **13** cyc.

`bsnes_cycles_probe.exe <dll> <rom> <startPC> <endPC> <expected>` runs the ROM
and compares bsnes's region CPU-cycle Δ to the authority's prediction. RESULT
(both): **MATCH** — bsnes 60 == authority 60; bsnes 13 == authority 13. The
recomp cost model (= what `emit_function` charges) is confirmed cycle-correct
against an accuracy-grade hardware reference, static AND dynamic, on
real-hardware-executed code. The "both can be identically wrong" trap is closed.

## REAL-ROM validation — recomp emit vs bsnes on actual game code (2026-06-28)

The synthetic test ROMs validate the *authority model*. This step validates
the **recomp's actually-emitted `cpu->cycles`** against bsnes over **real game
code** (Zelda: ALttP). Two new pieces:

1. **Recomp side (`runner/src/debug_server.c`, dev-only).** An always-on
   per-block cycle ring records `(pc24, cpu->cycles)` at every block leader
   (`debug_server_on_trace_block`, *before* the block's `cpu->cycles += const`
   charge — same fetch-boundary semantics as the bsnes `CPU::main` latch).
   Commands: `cyc_ring <path> [count]` dumps the ring; `cyc_anchor <0|1> <pc>` /
   `cyc_region` / `cyc_anchor_reset` are a live two-anchor latch. The Δ between
   two consecutive ring entries is exactly one block's emitted charge, and its
   two PCs bracket exactly that block's instructions in bsnes too.
2. **`ring_pick.py`** finds block transitions whose Δ is IDENTICAL across every
   occurrence in the ring (data-independent control flow), in two classes:
   *clean* (start does not self-loop) and *loop-exit* (start self-loops and
   exits to a different end — a conditional-branch block whose taken edge loops
   and not-taken edge exits; the recomp charges the +1 only on the taken edge).

**TIGHT latch (both sides).** The anchor latch records `start` on EVERY hit
(so the LAST start before `end` wins) and freezes `end` on its first hit after
a start has been seen. Taking the last start isolates the adjacent start→end
EDGE — exactly the single block the recomp ring measures — even when `start`
sits in a loop. This subsumes plain ordered latching and additionally nails
loop-exit edges (independent first-hit would span a variable iteration count;
plain ordered first-hit still spanned the loop). bsnes: `CPU::main` in
`bsnes_cycle_hook.patch`; recomp: `debug_server_on_trace_block`.

Workflow: launch the recomp (TRACE build) → `cyc_ring` dump from attract →
`ring_pick.py` → feed each `(start,end,recomp_Δ)` to
`bsnes_cycles_probe.exe <dll> <rom> <start> <end> <recomp_Δ>`
(`tools/cyc_watch/_realrom_diff.ps1` drives the batch).

**RESULT (Zelda attract, all 8 reachable regions):** recomp == bsnes EXACTLY on
every one — clean forward, a backward loop-branch, AND data-dependent loop-exit
edges, spanning 3 → 197 CPU cycles:

| region | class | recomp | bsnes |
|---|---|---|---|
| `$0092B2→$009328` | clean | 118 | 118 |
| `$009328→$009341` | clean |  40 |  40 |
| `$009341→$0092B2` | clean (loop back) | 3 | 3 |
| `$00814C→$008200` | clean | 197 | 197 |
| `$008719→$00874E` | clean |  17 |  17 |
| `$00811E→$00813C` | clean |  39 |  39 |
| `$008420→$008489` | loop-exit | 172 | 172 |
| `$0085FE→$00865C` | loop-exit | 150 | 150 |

The recomp's *emitted* cycle charges are confirmed cycle-correct against bsnes
on real game code — for clean, backward, and loop-exit edges alike.

**Remaining scope.** Bank-02/0C/1B attract PCs that bsnes does not reach within
the probe's frame budget report `NOT-HIT` (need a longer run or input to enter
those scenes). A region where an interrupt/DMA fires between the last `start`
and `end` would show bsnes > recomp by the handler cost — that is a true
interrupt-timing finding (Axis 3), not a model error.
