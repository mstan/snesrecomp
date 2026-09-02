# Widescreen pattern parity — engine doctrine

Mega Man X 1 (`MegamanXRecomp`) was the original surveyed, shipping 16:9
reference for this engine family. The same patterns have since been applied to
the X2 and X3 implementations. Getting X1 right took a long tail of small
corrections, and **every one of them is a pattern, not an address.** A new port
that reimplements widescreen from scratch will rediscover the same bugs in the
same order.

This document is the transferable part. Each entry is a defect X1 actually hit,
the invariant that prevents it, and how to check you have it. Treat it as a
checklist a port must satisfy — not as background reading.

`MegamanX2Recomp/docs/WIDESCREEN.md` and
`MegamanX3Recomp/docs/WIDESCREEN.md` hold the per-title measurements, release
gates, and relocated addresses; both point back here.

---

## Why per-game reimplementation is the wrong shape

X1's hooks live in `MegamanXRecomp/src/mmx_rtl.c` and are injected into the
*generated* C by `MegamanXRecomp/tools/apply_overrides.py`, which pattern-matches
emitted code and rewrites a value or flag in place:

```
/*WS-CULL*/  { cpu->_flag_C = MmxWsCullVerdictX((uint16)(_v12)); }
/*WS-OAM*/   { _v7 = MmxWsOamRightLimit(_v7); }
```

The *mechanism* is game-agnostic. Only the anchors are per-title. So the target
shape is a shared, profile-driven hook layer in the runner — a `WsGameProfile`
supplying gate, window bases, anchor slack, OAM limits, HUD slots and stage bias —
with X1 migrated onto it to prove parity, exactly as parallax was generalized out
of ActRaiser into `runner/src/parallax.c` + `ParallaxProfile`.

Until that extraction happens, a port must at least reproduce every invariant
below by hand, and say in its own doc which ones it has verified.

---

## The invariants

### P1. Scroll phase comes from the PPU, never from a WRAM camera mirror

**Defect:** margins reconstructed from the game's WRAM camera mirror are off by
one against the PPU's actual per-line scroll, producing the "pillar cut in half"
artefact at the margin seam.

**Invariant:** use `g_ppu->hScroll[]` / `vScroll[]` for pixel phase. A WRAM mirror
may be used to recover the *high* bits a wrapped PPU scroll has lost, but never
the phase.

**Check:** `get_ppu_state` scroll vs the WRAM mirror while scrolling; they will
disagree by one for a frame around each update.

### P2. Margin columns must be populated before they are displayed

**Defect:** first-visit margins show stale history or a wrapped copy of the
tilemap, because the game only streams a column when it reaches the native edge.

**Invariant:** seed off-native columns from the game's own retained level data
(`WsShadowPrefillTile`) without advancing the guest's camera, DMA queue, or
rolling-map bookkeeping. Read-only with respect to simulation.

**Check:** enter a room moving left and right; the outermost margin column must be
correct on the first frame it is visible.

### P2b. Bounded arenas reflect about the authored world edge, not the viewport edge

**Defect:** a single-screen arena (a fighter's stage) has a tilemap that is
authored only over a fixed pixel span, with tile 0 either side, and a camera
clamped so the native 256 view never leaves that span. Widening the view then
has two distinct regimes, and the existing policies each get one of them wrong:

- `PpuSetWidescreenLayerMirror`/`RepeatBand` fold the rendered scanline at
  screen x=0/255, so **mid-scroll** they discard the genuine authored art that
  the margins are now showing and replace it with a fold of the centre.
- Natural rendering is right mid-scroll but **at the camera clamps** runs the
  margins straight off the end of the authored span into tile 0 (or, on a
  smaller map, into a wrapped copy of the opposite side).

**Invariant:** reflect in *tilemap* space about the authored world's own edges,
not in screen space about the viewport's. Per pixel, with `x` = this line's
hScroll + screen x (map pixels, before the map's wrap): `x < left` samples
`2*left-1-x`, `x >= right` samples `2*right-1-x`, everything else renders
naturally. Columns the tilemap really authors — margin columns included —
survive untouched, and synthesis happens only where content ran out.
`PpuSetWidescreenLayerWorldMirrorBand(ppu, layer, y0, y1, left, right)`.

The band is per-layer and per-scanline because a raster-split status bar
re-points hScroll for its own lines, where a world bound is meaningless; every
other per-line policy (clamp band, layer mask, mirror/repeat/stretch, BG3
widen) therefore wins over this one, so a status band is excluded simply by
clamping it.

**Case:** GWED (`GundamWingEndlessDuelSNESRecomp`). BG1 is a 64x64 map authored
only in map px [64,448); the camera X clamp is [64,192], so the native 256 view
spans exactly [64,448) at the walls and the 43px margins are the only thing
that can leave the world. Lines 22-72 are a raster-split HUD band pinned to
hScroll 0 / vScroll 440 and are clamped instead. BG2 is a 256px skyline with
hScroll pinned to 0, where the map's own wrap already is the right answer.

**Check:** at both camera clamps the outermost margin column must be non-blank
and must equal the reflected source column; mid-scroll the margin must equal
what the tilemap authors there (i.e. must be byte-identical to the natural
render), which is the assertion that catches a viewport-space fold.

### P3. Periodic layers fold; world-anchored layers use history

**Defect:** treating a horizontally periodic layer (sky, repeating city glow) as
world-anchored history exposes stale content at the seam until it scrolls through.

**Invariant:** prove a per-row period from natively-displayed columns and fold
(`WsShadowSetPeriodicFold`); only use captured history for rows that are genuinely
world-anchored. Layer serving order: periodic fold, then world-keyed history, then
plain map wrap.

### P4. Presentation keys to the scroll actually rendered this frame

**Defect:** the game advances its WRAM shadow before the corresponding PPU write,
so keying margin history to the shadow makes margins alternate one frame early.

**Invariant:** key any presentation-only history to the values the PPU rendered
with, and unwrap them into a monotonic world coordinate yourself.

### P5. The HUD gate must be a real game-state discriminator

**Defect:** X1 gated partly on `$00C3`, an HDMAEN mirror. Any effect that
temporarily enabled extra HDMA channels (Spark Mandrill's light streaks) made the
HUD snap back to native placement mid-effect.

**Invariant:** gate on a byte that means "in live stage gameplay" and nothing
else. Verify it across every mode — menus, intro, boss intro, mode 7, pause.

**Check:** watch the candidate byte with `set_wram_watch` while moving through all
modes before trusting it.

### P6. A game-mode byte proves the mode, not liveness

**Defect:** in-stage cutscenes report the gameplay mode while the player pipeline
is idle, so a mode-only gate enables widescreen behavior during scripted scenes.

**Invariant:** where liveness matters, require an observable consequence, not a
mode byte alone.

### P7. Cull windows widen symmetrically, with the base window preserved

**Defect:** culling keyed to the native edge deletes objects still visible in the
margins.

**Invariant:** `carry = (v + m) >= (base + 2*m)`, keeping each routine's own base
window. Enemies and projectiles have *different* bases (X1: `0x40`/`0x180` and
`0x20`/`0x140`) — do not unify them.

### P8. The OAM emitter is a SECOND gate, and its compare is unsigned

**Defect — the most easily missed:** widening the metasprite X *reject limit* is
not enough. The reject is a single **unsigned** compare, so negative screen X
wraps high and always rejects; sprites still vanish at the native left edge no
matter how far the limit is widened.

**Invariant:** widen the limit **and** replace the compare:

```c
uint16 WsOamXReject(uint16 x_plus_16, uint16 widened_limit) {
  if (x_plus_16 < widened_limit) return 0;                   /* right window */
  if (m && x_plus_16 >= (uint16)(0u - (uint16)m)) return 0;   /* left margin  */
  return 1;
}
```

The PPU's 9-bit OAM X path already renders negative and 256+ coordinates.

**Check:** an enemy must remain drawn while straddling both margin edges, not just
the right one.

### P9. Spawn anchors need margin **+ one column** of slack

**Defect:** an anchor of exactly the margin lands spawns on the outermost
*visible* wide column — visible pop-in.

**Invariant:** `margin + 32`. Spawned objects still sit inside the widened cull
window's hysteresis on either side.

### P10. Widened spawning must not widen progression records

**Defect:** widening the spawn anchor for *everything* fires camera staging,
minibosses and stage controllers early, changing progression and RNG.

**Invariant:** dual pass. The widened pass admits **only ordinary enemy records**;
a second pass at the **unmodified 4:3 anchor** admits everything else. Per-record
flags make an already-created enemy a no-op in the native pass.

### P11. The native pass must be a balanced synthetic call

**Defect:** re-entering the record walk without preserving guest state corrupts
the stack.

**Invariant:** save/restore `CpuState` around `cpu_dispatch_call_pc`, and restore
the anchor bytes afterwards so the caller's remaining logic sees what it wrote.

### P12. Large objects need their activation distance widened

**Defect:** a big sprite's controller wakes only when its centre reaches the
widened edge, so its outer tiles pop in.

**Invariant:** widen the activation distance by `margin + 32` as well as the
spawn anchor.

### P13. Stage-trigger lead must not exceed the margin

**Defect:** firing tilemap/CHR staging too early swaps a section's shared OBJ
graphics before the old section has left the screen — garbled CHR.

**Invariant:** bias the trigger line *into the travel direction* by the margin
only, and keep the two direction conditions complementary at every instant so
there is no fire-loop. Bias only X-axis staging watchers; Y staging has no
vertical margin, and other trigger classes (camera locks) must fire natively.

### P14. Every widening gets its own kill-switch

**Invariant:** independent env gates (X1: `SNESRECOMP_WS_SPAWN`,
`SNESRECOMP_WS_STAGE`) so one misbehaving part can fall back to authentic 4:3
while the rest stays active. A single global toggle makes bisection impossible.

### P15. Renderer-side previews must stand down when real objects arrive

**Defect:** a margin preview that composites a static duplicate over a
now-genuinely-spawned enemy.

**Invariant:** a predicate like `WsRealSpawnActive()` that disables any
preview/prefill layer once real spawning populates the margins.

### P16. Simulation must be untouched, and 4:3 must stay bit-identical

**Invariant:** camera, collision, AI, RNG and save-state data unchanged. 16:9 is
presentation plus spawn/cull bounds.

**Check — this is the release gate:** capture frames with the enhancement OFF
before and after the work and diff them. Use the PPU frame-diff tooling; disable
widescreen and any sprite-limit removal for the comparison.

---

## Order of work

Doing these out of order wastes time, because a later step's symptoms mimic an
earlier step's bug:

1. **P16 gate first.** Establish the 4:3 baseline capture before touching anything.
2. **P5/P6** — find and prove the gameplay discriminator. Everything else keys off it.
3. **P1-P4** — background layers. Visible, self-checking, no simulation risk.
4. **P7 + P8** — cull and the OAM emitter together. Fixing cull alone looks broken
   because the emitter still clips.
5. **P9-P12** — spawning, with the dual pass from the start rather than retrofitted.
6. **P13** — staging bias last; it interacts with everything above.

## Status

| game | state |
|---|---|
| Mega Man X 1 | surveyed and shipping; source of every entry above |
| Mega Man X2 | HUD, exact BG margins, object windows, dynamic record frontier, and weather margins implemented as a default-disabled mod; broad stage playtest remains pending |
| Mega Man X3 | 342x224 rendering, HUD, exact BG margins, and widened activation/culling implemented; authentic 4:3 remains the default |
| others | use title-specific strategies until their reusable patterns are catalogued here |

A port claiming widescreen support should record which of P1-P16 it has verified,
and by what measurement.
