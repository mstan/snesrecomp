# Asset dumping + function identification tools (plan)

Status: **phases 0–3 built** (2026-08-26) · tools live in
**`retcomm-studio/tools/snes_analysis/`** (maintenance moved there by
decision, mirroring `tools/psx_analysis/`; the architecture rule below is
unchanged) · phase 4's Studio Assets tab remains open · phase 0 landed as
snesrecomp `87c2109`

Delivery notes against the plan: phase 0.1/0.3 turned out to already exist
in-tree (the write rings carried func/stack stamps and a shared seq), so
phase 0 reduced to the interpreter scope push and the FramePpuSnap decoder
inputs. The live soak against MetalWarriorsSNESRecomp validated the whole
chain — capture, decode, and an attribution draft at $00:8182 that the
project's symbols already name I_NMI — and caught one capture bug (the
history ring's newest frame is live−1).

Goal: dump SNES assets **as composed** (tiles with the right palette, layers
with the right scroll, sprites with the right size/palette, samples as audio)
and attribute what is on screen to the **source function that drew it**, so
naming a routine in `symbols.toml` starts from a click instead of a trace
session.

---

## 1. The architecture rule, borrowed from the PSX Frames tab

retcomm-studio's PSX Frames tab states its division of labour plainly
(`src/studio/studio_frames.hpp`): the framework's Python tools own the debug
protocol, the decode, and the render; they write JSON and PNG; **Studio is a
viewer and a launcher**. Nothing in Studio decodes a packet, so a headless
capture and the tab can never disagree about what a frame contained.

The SNES side adopts that rule unchanged:

| Owner | Responsibility |
|---|---|
| `snesrecomp` runtime | rings + dump commands (exists), writer attribution (Phase 0) |
| `snesrecomp/tools/assetdump/*.py` | protocol, decode, attribution join; writes JSON/PNG/WAV artifacts |
| Studio SNES Assets tab | launch the tools, view the artifacts, one live status connection |

Everything downstream of capture works **offline from the artifact bundle** —
a bug filed with a bundle attached needs nothing still running.

## 2. What already exists (verified in-tree)

The capture side is largely solved; the gap is attribution and presentation.

- **Debug server** (TCP, ~197 commands, built only with
  `SNESRECOMP_ENABLE_TRACE=ON`): `screenshot` (copies the *presented* buffer,
  widescreen included), `dump_vram` / `dump_cgram` / `dump_oam` /
  `dump_apu_ram` / `dump_ram`.
- **Frame history ring**: `FrameRecord` keeps, per frame, full 128 KiB WRAM,
  full 64 KiB VRAM, CGRAM, OAM + highOAM, and CPU/PPU/DMA/IRQ snapshots —
  queryable by frame number (`dump_frame_vram <N>` …). Capture therefore
  **reaches backwards after the fact**; the game is never paused to observe
  it, which is the same RULE-0 stance the PSX capture tool documents.
- **Write-attribution rings**: `get_vram_trace`, `last_vram_write_to`,
  `oam_write_get` / `oam_render_get` (writes and render-reads share one
  monotonic seq), `wram_writes_at` (old **and** new values),
  `dispatch_log_get` (`pc24`, `source_pc24`, `found`, `frame` — `found:0` ran
  interpreted).
- **`g_recomp_stack`** (depth 64): live stack of recompiled function names.
  **AOT frames only** — verified 2026-08-26: `interp816.c` and
  `interp_bridge.c` contain zero push sites, so interpreted frames are absent
  and a whole-program-LLE port (exactly what the scaffolder produces) has an
  *empty* stack.
- **Host-overlay extraction** (`docs/HOST_OVERLAY_EXTRACTION.md`): the PPU can
  export selected BG/OBJ regions as composed ARGB without touching emulated
  state — decode/scroll/windows/mosaic/palette/brightness applied.
- **Studio**: platform plugin registry, a worker-threaded `DebugClient`
  pattern, and the Frames tab as the model. Functions/Frames are PSX-only
  today *by design* — absent rather than empty under SNES.

## 3. Phase 0 — close the attribution gaps (runtime, small C)

Everything else joins existing data; this phase makes the joins possible.

1. **Stamp writer identity onto the VRAM / OAM / CGRAM write rings**: top of
   `g_recomp_stack` (name pointer — they are string literals, no lifetime
   issue) plus the writing `pc24`. Field additions to existing ring entries;
   zero cost when trace is off.
2. **Interpreter scope push.** `interp_tier_dispatch_balanced` already
   receives `target_pc24` and is already the interp-side scope boundary for
   the write-log; push a synthesized name there (`interp@$BB8CB5`, interned)
   and at the `interp_bridge_run*` entries so whole-program LLE attributes
   too. This closes the LLE-first hole — without it, new ports get nothing.
3. **One clock.** OAM writes and render-reads already share a monotonic seq;
   VRAM/CGRAM ring entries carry only frame numbers. Stamp the same seq onto
   them so pixel-accurate joins do not straddle frame boundaries.
4. **Decoder inputs audit.** Confirm `get_frame_extended`'s `FramePpuSnap`
   carries everything decode needs per frame — BG mode, per-BG tilemap/char
   bases and scroll, OBJ char base and size select, main/sub enables, CGWSEL.
   Extend the snap if anything is missing. Acceptance: every decoder input in
   Phase 2 is queryable **for frame N**, not just "now".

Doctrine note: attribution rides always-on rings, queried while the game free
runs. No pause/step anywhere in the workflow.

## 4. Phase 1 — capture (`tools/assetdump/snes_frame_capture.py`)

One command, one self-contained bundle:

```
python3 tools/assetdump/snes_frame_capture.py --tag bad --out analysis/frames
python3 tools/assetdump/snes_frame_capture.py --frame 41230 --frames 5 --tag bad
```

- Pulls, for the chosen frame(s) from the ring: screenshot PNG, VRAM, CGRAM,
  OAM/highOAM, PPU regs, optionally WRAM, plus the write-trace and
  dispatch-log windows covering those frames.
- Writes `<tag>.json` (versioned `snes-frame` manifest: frame numbers, ROM
  identity digests, tool versions) + raw binaries + `<tag>.png`.
- **Licensing:** bundles are derived from ROM data. The output dir is
  gitignored (scaffolder template gains the entry) and bundles are never
  committed — same posture as `src/gen/`.

## 5. Phase 2 — decoders (`snes_asset_decode.py`, offline)

Input is a bundle, never a live game. Each output carries provenance (frame,
ROM digest, generating tool+version).

| Asset | Output |
|---|---|
| CGRAM | palette swatch PNG + JSON (RGB555→RGB888) |
| Tiles | 2/4/8bpp tilesheets per char base, rendered per candidate palette |
| BG layers | BG1–BG4 composed PNGs using the frame's mode/scroll/tilemap (Mode 7 deferred — flagged, not silently wrong) |
| Sprites | 128 OAM crops with correct size/palette/flip; sprite sheet + per-slot JSON |
| Audio | BRR samples from APU RAM via the DSP `DIR` table → WAV per sample |

Two consumers, deliberately separated: **identification** (seeing that a
graphic *is* the pause menu is what lets you name `DrawPauseMenu`) and
**enablement** (the overlay-extraction path exists to substitute
higher-resolution art; these dumps are its input for HD packs and widescreen
work).

## 6. Phase 3 — attribution (`snes_frame_attribute.py`, offline)

The point of the whole exercise:

- For every OAM slot and touched VRAM region in frame N: the last writer —
  function name from the stamped stack, or `interp@$addr`, or bare `pc24` —
  with write counts aggregated over the captured window.
- Emits `attribution.json`: asset → writer, confidence, first/last frame seen.
- Emits **draft `[[func]]` suggestions** for unnamed addresses, with an
  evidence note (`"writes OAM slots 12–15 every frame — player sprite?"`).
  Drafts are written to a suggestions file, **never applied**: discovered
  coverage is triaged by a human into config, per workspace doctrine. The
  same rule the tier2/dispatch flow already follows.

## 7. Phase 4 — Studio (SNES Assets tab + CLI)

- **SNES `DebugClient`** sibling of the PSX one (same worker-thread rule — a
  hung runtime never stalls the UI; different wire protocol).
- **Assets tab** (SNES-only, mirroring how Frames is PSX-only): launch
  capture/decode/attribute; gallery of sprites/tiles/layers/palettes; click
  an asset → its attribution → **"append draft to symbols.toml"** into a
  clearly-marked generated block for the human to promote.
- **Port preflight**, the SNES `DebugToolsInfo`: read the project's CMake
  cache and answer "will this build even open the debug port"
  (`SNESRECOMP_ENABLE_TRACE` is OFF in production builds) — the first
  question of every "why won't it connect".
- **CLI**: `retcomm_studio_cli.py --platform snes assets capture|decode|attribute`
  so bulk/headless runs need no GUI; unsupported subcommands keep refusing
  early, as the SNES platform gate already does.
- **Provenance rule** carried over from `studio_debug.hpp`: dynamic evidence
  is labelled and never silently merged into a static claim. "Observed
  writing OAM in 4,812 frames" and "named in symbols.toml" stay different
  columns.

## 8. Phase 5 — later, built on the same artifacts

- **Visual blame**: click a pixel in the screenshot → layer/OAM slot → writer
  (the keystone use-case; everything above is its supply chain).
- **Oracle diff overlay**: snesref's WRAM/VRAM traces vs the recomp's, first
  divergent frame rendered with the divergent sprite highlighted.
- **WRAM struct discovery**: cluster `wram_writes_at` by stride to recover
  actor-table layouts; name fields from writer attribution.

## 9. Verification strategy

- **Decoders**: synthetic fixture bundles (known tile/palette patterns
  hand-assembled, like the scaffolder's fixture ROM) with byte-exact expected
  PNGs; registered in `tests/run_tests.py`. Mutation-check the bit-plane and
  palette-index math — the classic failure is a plausible-looking wrong
  decode.
- **Attribution**: fixture with two known writers (one AOT, one interp) →
  assert both names land; mutation-check that dropping the interp push is
  caught (that is the bug Phase 0.2 exists to fix).
- **Runtime stamps**: extend the C suite with a ring-stamp test in the
  existing `tests/` style; trace-off build must be byte-identical in behavior.
- **Live smoke**: one capture against a real title with a trace build
  (MetalWarriors or MMX) — CI-excluded, doc'd as a manual soak step.

## 10. Risks and open questions

- **Mode 7, hires, interlace**: deferred; decoders must *say so* per frame,
  not emit garbage.
- **Coprocessor writers** (SA-1 / Super FX / Cx4 DMA into VRAM-bound
  buffers): their writes may not pass through the stamped paths — attribution
  will say `unknown`, which is honest; scoping their attribution is its own
  later task.
- **Ring eviction**: a busy frame can evict the write you chase; the
  aggregate counters exist precisely because of this. Capture windows > 1
  frame are the mitigation.
- **Perf of stamping**: name-pointer + u32 per ring entry, trace builds only;
  measure, but expected negligible.

## 11. Order and sizing

Phase 0 is small C and unlocks everything; 1–3 are the bulk and are plain
offline Python against a documented protocol; 4 mirrors code Studio already
has for PSX. 0 → 1 → 2 ship value on their own (dump + identify by eye);
3 → 4 deliver the click-to-function loop.
