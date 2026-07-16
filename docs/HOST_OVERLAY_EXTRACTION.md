# Host-overlay extraction

The PPU can export selected, already-rendered SNES graphics into transparent
ARGB surfaces without modifying emulated VRAM, OAM, WRAM, registers, DMA, or
savestate data. This is a renderer capability; each game still owns the policy
that identifies a HUD, portrait, logo, menu panel, or other promotable graphic.

This capability is **opt-in and additive**. With no surface bound and no capture
rectangle configured, every source is a deterministic no-op: each layer is drawn
straight into the normal composition buffer and the sprite/backdrop paths behave
exactly as before, so authentic/headless/oracle output is byte-identical. It was
ported (additively, preserving this engine's existing widescreen layer-policy
API) from Derrick Gold's ActRaiser fork; see the attribution below.

## Boundary of responsibility

| Owner | Responsibility |
|---|---|
| PPU runner | Isolate BG1-BG4/OBJ pixels, apply tile decode, scroll, windows, mosaic, palette and master brightness, optionally omit the captured rectangle from the game framebuffer |
| Game policy | Decide the source, screen-space rectangle, OAM slots, and game states in which capture is valid |
| Host frontend | Crop pieces from the surface, anchor/scale them, substitute higher-resolution art, and choose final presentation order |

The runner deliberately does not depend on SDL, OpenGL, or a particular host
layout. Independent post-upscale composition remains a frontend operation.

## Lifecycle

Bindings are persistent host resources; capture rectangles are per-frame game
policy:

```c
PpuClearOverlayBindings(ppu);
PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3,
                      bg3_argb, framebuffer_pitch);
PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj,
                      obj_argb, framebuffer_pitch);

/* Once per emulated frame, before scanout. */
PpuClearOverlayCaptures(ppu);
PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg3,
                     0, 0, 256, 40,
                     kPpuOverlayFlag_RemoveFromGame);
PpuSetOverlayCapture(ppu, kPpuOverlaySource_Obj,
                     0, 0, 256, 40,
                     kPpuOverlayFlag_RemoveFromGame);
PpuSetOverlayOamRange(ppu, 0, 4);
```

Each source owns one bounding rectangle and one full-frame output surface. A
frontend may crop several disjoint graphics from that rectangle. Separate
sources can be captured simultaneously. Passing flags `0` makes a diagnostic
copy while retaining the source in the normal framebuffer;
`RemoveFromGame` promotes it by omitting the rectangle from both the SNES main
and subscreen paths.

Coordinates are visible screen space after scroll/window/mosaic processing.
The authentic screen is `x=[0,256)`; widescreen margins may use negative X or X
greater than 255. Output surfaces share the game framebuffer's pitch and
coordinate system, so authentic X zero is stored after the surface's left
centering budget.

OBJ capture additionally requires an OAM range. The runner treats slot identity
as opaque; the game must validate that the slots still represent the intended
graphic before selecting them. Rectangle clipping is per pixel, so partially
intersecting sprites retain their non-captured pixels in the normal OBJ plane.

## Rendering semantics

- Transparent BG/OBJ pixels remain alpha zero. Palette and master brightness
  are resolved before export.
- BG capture uses an isolated priority buffer. Outside a promoted rectangle,
  the isolated layer is merged back using the normal per-pixel priority word.
- The exported ARGB comes from the main-screen layer pass. The subscreen pass
  is isolated too when removal is requested, but is not exported; promoting a
  layer used only for subscreen color math needs a future screen-selection or
  intermediate-composition extension.
- Promotion is applied to main and subscreen so removed graphics cannot leave a
  color-math ghost underneath the host copy.
- Bindings survive `ppu_reset`; capture policy does not. A NULL binding is a
  deterministic no-op and preserves pure-headless/oracle output.
- The host overlay is above the already-flattened framebuffer. HUDs and topmost
  UI are therefore direct. Replacing scenery that must remain behind sprites or
  foreground BG priority requires exporting those occluding planes too, or a
  future intermediate-composition hook.

## Current coverage

The descriptor namespace includes BG1-BG4 and OBJ. This engine's renderer draws
SNES Modes 1, 2, 3, and 7; capture is currently wired for **Mode 1 BG1/BG2
(4bpp) and BG3 (2bpp), and Mode 7 BG1**. Modes 2 and 3 render normally but do
not yet feed the overlay path; wiring them (and BG4, once a mode that draws it
lands) is a mechanical follow-up that reuses the same
`PpuBeginBackgroundOverlay`/`PpuFinishBackgroundOverlay` seam. The old PPU
renderer (`PpuDrawWholeLineOldPpu`) does not implement host-overlay extraction.

This port leaves the engine's existing widescreen layer-policy controls
(`PpuSetWidescreenLayerClamp`/`Mirror`/`Repeat`, the clamp/repeat bands, the
margin gap, the BG3 widen/HUD-split, and the Mode-2 layer capture) fully intact;
overlay extraction is an orthogonal, independently opt-in capability.

## Attribution

Ported from Derrick Gold's ActRaiser SNES recompilation fork
(`DerrickGold/snesrecomp`, commit `c6ad43a` "PPU: generalize host overlay
extraction"). ActRaiser is the first consumer: it captures the authentic BG3
status rectangle and a validated four-slot OBJ graphic, then performs its
game-specific left/center/right composition after SDL has upscaled the world.
