# Frame-Model Hosts

This note is for hosts that drive game time with their own frame or scanline
loop and call `interp_bridge_run_loop()` or related bridge entry points
directly. Hosts that use `RtlRunFrame()` normally inherit the framework's timing
contracts and do not need this integration layer.

## Beam Ownership

Use one beam owner. The framework updates `snes->hPos` and `snes->vPos` from
`snes_sync_master_clock()` while interpreted code runs, so a host that also
advances an independent beam can split a frame between two clocks. Device work
derived from only the host-side clock can then miss edges that the bridge-side
clock crossed.

`interp_bridge_run_loop()` and related LLE bridge entry points are therefore not
"CPU only" helpers. While they execute guest instructions, they also advance the
shared `Snes` beam to the interpreted CPU master clock. Prefer scheduling host
device work from that shared `Snes` beam state, or keep a single explicit
host-owned beam and audit every framework edge that must be called manually.

Frame-model hosts that own HBlank/VBlank edges are responsible for hardware
work normally driven by the framework timeline, including:

- `ppu_checkOverscan()` and `ppu_handleVblank()` at VBlank entry.
- NMI and automatic joypad-poll handling at the documented hardware edge.
- `dma_initHdma(snes->dma)` on frame wrap and `dma_doHdma(snes->dma)` once per
  visible HBlank when using LLE HDMA.
- APU catch-up at a cadence that does not depend on one frame-sized catch-up
  call.

## APU Pacing

`snes_catchupApu()` has a per-call runaway clamp. That guard assumes callers
catch up regularly; a host that batches an entire frame into one call can discard
part of the requested SPC time. Frame-model hosts should catch up the APU at
smaller timing slices or use the framework frame runner that owns the audio
timeline.

Ordinary SNES interpreter fallback still applies relative bridge catch-up for
interpreted CPU time. Only the active SA-1 absolute frame timeline suppresses
that relative catch-up, because its port accesses and frame boundary sync to the
same guest timestamp. Hosts outside that model should not assume bridge exits
alone have advanced all deferred SPC time; flush or catch up at explicit
integration points.

## Bridge Hooks

`interp_bridge_set_pre_opcode_hook()` is the supported pre-opcode extension
point. The older `interp816_opcode_hook` symbol is a legacy no-op and is not
called by the bridge.

`interp_bridge_run_interrupt()` expects the caller to have already pushed the
hardware interrupt frame, for example with `cpu_push_interrupt_frame_at()`. If
the host has not materialized that frame, use the normal resume path after
pushing it rather than entering the interrupt body directly.
