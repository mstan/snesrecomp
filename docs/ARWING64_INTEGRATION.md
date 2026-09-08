# Arwing64 engine integration (2026-09-07)

The integration combines the accepted `58b02f7` Arwing64 branch with upstream
`5553226`. It preserves the newer S-DD1, localization, OAM motion-history and
runtime diagnostic optimizations. Host meshes, audio replacement and private
Super FX presentation replay remain opt-in. Replay writes to a private copy;
the console's VRAM and Super FX RAM remain authoritative.

The merge restores public interfaces removed by the earlier vendor import:
the title-owned Enhanced frame callback, `game` debug commands, and title-owned
tier-2 capture opt-in. Trace builds again include their crash-report dependency.
The existing 800-pixel viewport capacity, 16-bit margin fields, Mode-2 OPT
sampling, half-color composition and OBJ capture selection are preserved.
Moving OBJ grace uses upstream's new history with the restored hint check.

Validation includes host mesh, mod audio, guarded patch, PPU sprite/overlay,
Super FX opt-in/replay, dispatch, APU guest time, diagnostic gates and tier-2
capture tests. The PPU composition differential matches the accepted branch
(`436319d369c4a1e3`). Star Fox builds with tracing enabled and disabled.

StarFoxSNESRecomp continues to pin `58b02f7`, the engine version used for the
owner's accepted build and guest-state comparisons. This merge does not repair
the separate CPU/raster-IRQ regression tracked by `beads-8wg.2.27`; that issue
still requires cross-game timing validation before changing Star Fox's pin.
