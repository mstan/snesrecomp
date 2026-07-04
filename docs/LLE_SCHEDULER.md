# LLE scheduler — path to "rich cfg × LLE" (compiled tasks, real scheduler, no HLE)

Status 2026-07-02. Owner direction: **every game moves to the LLE scheduler tier with a
rich (fully compiled) cfg; the C-host HLE scheduler and its per-game task-PC tables are
retired.** This doc records where each configuration stands, why compiled task bodies
don't yet run under LLE, and the fiber-free design that closes the gap.

## Where each configuration stands

| game | cfg | scheduler | tasks run as | state |
|------|-----|-----------|--------------|-------|
| MMX USA | rich | HLE (default) | compiled (fibers) | shipped, validated |
| MMX USA | rich | LLE (`SNESRECOMP_MMX_SCHED_LLE=1` + immediate ports) | **compiled via bounce** (fiber-free) | **DONE 2026-07-03**: gates 1-3 green (A-vs-A 0/400; bounced-vs-interp 0/4000 modulo the masked `$02FF` residue byte; LLE-vs-HLE ppu/dma 0/4000); live 3×60.2 fps clean |
| Rockman X JP | enriched (134 funcs, tier-2 round 1) | LLE (default) | interpreted (bounce opt-in: `SNESRECOMP_LLE_BOUNCE=1`) | bounced-vs-interp + A-vs-A(bounce) green 2026-07-03; default flip awaits gameplay coverage + sign-off |
| next | — | LLE default everywhere, HLE removal | — | needs owner sign-off |

The 2026-07-02 campaign proved the LLE tier itself is sound: LLE-vs-HLE guest video
state is bit-identical for 4000 frames headless, and the live wedge/garble/chug was
three APU-domain wall-time defects (torn word port stores, per-opcode lock contention,
deferred-port stalls), all fixed in `22734b2` — **not** a scheduler-logic or cfg-richness
problem. See memory/commit messages for the mechanism details.

## Why tasks still interpret under LLE (the bounce suppression)

`_interp_run_core` (interp_bridge.c) in yield mode (`yield_pc != 0`, i.e. the
`interp_bridge_run_scheduler` frame) never bounces a JSR/JSL to a compiled body:

1. The scheduler's yield primitive (`$00:8100` family) is a **coroutine switch**: it
   consumes the caller's return frame, stores a resume PC into the slot's `$32/$33`,
   saves S, restores the scheduler loop's S, and BRAs back into the loop — it never
   returns to its caller. Bouncing any body that transitively reaches it under the
   paired-call ABI (which assumes the callee returns) corrupts the stack.
2. Under HLE those sites are `hle_func`'d to stubs (`HleMmxYieldOneFrame` /
   `HleMmxYieldNFrames` / `HleMmxYieldVblank` / `HleMmxTaskDie`) that suspend the
   hosting **fiber** (`mmx_host_yield` → `SwitchToFiber`). Under LLE there are no
   fibers — a bounced compiled body reaching those stubs would suspend a fiber that
   does not exist.

So today the whole scheduler frame interprets. Correct, but it ships an interpreter for
the game's entire per-frame logic — explicitly rejected for production. Compiled bodies
are the point of the recompiler.

## Design: fiber-free yield via NLR unwind + interpret-the-real-primitive

**IMPLEMENTED 2026-07-02** (engine `interp_bridge.c`/`cpu_state.h` + MMX
`gen_stubs.c`). The build refines the original sketch in one important way: the
LLE-aware stubs do NOT hand-replicate the primitives' guest-state effects — they hand
control back to the interpreter *at the primitive's real ROM entry*, so the actual
coroutine switch executes byte-exact by construction. Zero effect-duplication to keep
in sync with the ROM.

Ground truth from the `$8099` disassembly (USA; JP byte-identical): the loop resets S
to `$02FF` each pass (`$8099`), spins on `$0B9D` (`$80A1`), walks slots at `$80AB`;
state-1 fresh-dispatches via `LDA $37,X ; XBA ; LDA $36,X ; TCS ; JMP ($0032,X)`
(`$80DA`); state-2 resumes via `REP #$30 ; LDA $34,X ; TCS ; PLP ; PLY ; PLX ; RTS`
(`$80E9`). The yields (`$8100` one-frame, `$810C` N-frames, `$8127` vblank-yield tail)
all do `PHX ; PHY ; PHP`, reload X from `$A0`, store state+countdown 16-bit into
`$30/$31,X`, save S via `TSC → $34/$35,X`, and re-enter the walk. (`$34/$35` is the
yield-time saved S; `$36/$37` is only the *fresh-install* entry-S — the original sketch
had this wrong.) `$80F8` task-die is `SEP #$30 ; LDX $A0 ; STZ $30,X ; BRA $80B9`.

**1. LLE-aware yield stubs** (`gen_stubs.c`). Each stub tests
`interp_bridge_in_lle_scheduler()` (a scheduler-frame depth counter maintained by
`interp_bridge_run_ex2` for `yield_pc != 0` frames — visible through nested tier-2
frames). In LLE context it calls `interp_bridge_lle_yield_unwind(cpu, PB:realEntry)`
instead of `SwitchToFiber`; cpu is left exactly as the compiled callsite made it (JSR
frame already pushed for JSR-reached primitives, live A for `$810C`, live X for the
`$80E6` dispatch). One optimization keeps the hot path compiled: `$8121`'s NO-YIELD
case (`BIT $0B9D ; RTS`, the decompressor's every-32-units check) is modeled in the
stub byte-exact (BIT N/V/Z at live M width + frame pop) so only the actual yield
unwinds.

**2. NLR unwind back to the bridge.** `interp_bridge_lle_yield_unwind` arms a pending
unwind (+ resume PC) and returns `RECOMP_RETURN_LLE_UNWIND_BASE` (0x40000000). Every
emitted callsite already propagates non-NORMAL returns (`return _r - 1`); the sentinel
is so far above any genuine SKIP_N that per-level decrements can't decay it. Nested
non-scheduler bridge frames (tier-2 gap runs) end on the unwind and their
`interp_tier_*` helpers re-emit the sentinel, so the unwind crosses interleaved
compiled/interpreted frames of any depth. The scheduler frame's bounce site consumes
the request and CONTINUES INTERPRETING at the primitive's real entry — the interpreter
performs the real coroutine switch and walks on. (The unwind also swallows any
tail-armed `cpu_tailcall_inherit_return_context` the hle wrapper never took, so the
next bounce can't adopt a stale `_entry_s`/`_hrv`.)

**3. Resume next frame is already native.** The interpreted loop's own resume
(`LDA $34,X ; TCS ; PLP ; PLY ; PLX ; RTS`) pops the compiled callsite's pushed frame
and lands at the post-JSR guest PC; the interp executes the task's tail from there and
bounces subsequent calls back into compiled bodies. Mid-function entry under interp is
proven machinery (`interp_bridge_resume_task`).

**4. Bounce suppression lifted.** Yield-mode bounces are now enabled by default;
`SNESRECOMP_LLE_BOUNCE=0` restores interpret-everything (the A/B differential lever —
bounced vs interpreted must be guest-state bit-exact). No denylist proved necessary:
all non-returning machinery is JMP-reached (never bounced — the interp only intercepts
JSR/JSL/JSR(abs,X)) or hle_func'd (handles itself via the unwind). A JSR arrival at a
yield primitive from *interpreted* code simply round-trips through the stub and
resumes interpreting at the same PC — correct, one redundant hop.

**No fibers. No per-game task table.** The only per-game knowledge is what the rich cfg
already encodes: which PCs are the yield/die primitives (`hle_func` directives that
minimal cfgs need anyway) — and those stubs name their own real ROM entries.

## Validation plan (the differential bonus)

Rich-LLE-bounced vs rich-LLE-interpreted is a **whole-game codegen differential**: both
sides run the identical guest instruction stream under the identical scheduler, one
executing task bodies compiled, the other interpreted. Under the co-sim shared APU
clock (`SNES_COSIM_APU_SHARED=1`) any cpu/ram divergence is a recompiler-semantics bug,
drillable to the exact opcode with `SNES_COSIM_SYNC_PC`. Gates:

1. A-vs-A determinism (bounced) — 0 divergence.
2. Bounced vs interpreted — cpu/ram/ppu bit-exact over title + attract + demo.
3. Live: full-speed, clean render, multiple restarts (the wall-time race class).
4. Then flip defaults per variant (`MMX_SCHED_LLE_DEFAULT=1` + immediate ports
   becomes the build default) — USER SIGN-OFF required before changing USA's shipped
   default; HLE stays available via env until removal is approved.

## Also required for "LLE everywhere"

- **Immediate APU ports under LLE** (deferred scheduling stalls interp'd handshakes —
  the JP gate-#3 / USA copyright-freeze class). When LLE becomes a build's default,
  `SNESRECOMP_APU_IMMEDIATE_PORTS_DEFAULT=1` goes with it. The SMW missed-SFX deferred
  path remains the default for HLE builds only.
- **JP cfg enrichment**: JP is minimal; grow it toward rich with the tier-2 gap
  manifest + `tools/tier2_ingest.py` worklist once bounce is enabled (bounce is what
  makes the manifest see un-compiled task callees as tier-downs).
- **Audio validation** (issue #4 lineage) is still the open acceptance bar per variant.
- SMW / Zelda / SM adopt the same pattern when their scheduler idioms are LLE'd
  (SM's single-fiber WaitForNMI HLE is the analogous seam).

---

## SMW interpret-EVERYTHING floor: the dead-scratch dispatcher gap (2026-07-04)

Gate 2 above ("bounced vs interpreted — cpu/ram/ppu bit-exact") holds for SMW **only
after** you account for one structural, benign gap. The `smw_cosim` LLE bounce-vs-
interpret gate (A = `SNESRECOMP_SMW_SCHED_LLE=1`; B = same + `SNESRECOMP_LLE_BOUNCE=0`,
both `SNES_COSIM_APU_SHARED=1`) forks at **frame 2** and never re-converges on the raw
`cpu`/`ram`/`ppu` hashes. This was drilled to root cause and proven benign — it is **not**
a codegen semantic bug.

### Root cause — the `$0086DF` static-dispatch optimization elides dead scratch

SMW's `$00:86DF` is a **stack-based jump-table dispatcher**: `JSL $0086DF` is followed by
an inline `.dw` target table, and the routine `PLA`/`PLY`s its own return address off the
guest stack, indexes the table by the game-mode value in A, loads the target into A, writes
direct-page scratch `$00–$03`, and `JML`s to it.

The recompiler registers `$0086DF` as a **dispatch helper** (see `bankNN.cfg`
`# JSL inline dispatch trampolines` blocks; e.g. `bank03.cfg`). The decoder reads the
inline table at compile time (`decoder.py` `dispatch_helpers`, stamps
`insn.dispatch_entries`) and codegen emits a **direct static dispatch to the target**,
marking the JSL a terminator. The dispatcher body therefore **never runs on the compiled
side** — so its transient register/scratch side effects (`A` ends holding the jump-target
address, `$00–$03` scratch) are **elided**. The interpreter runs the real routine, so it
reproduces those values faithfully (matching hardware).

Net: codegen is **control-flow-faithful and live-state-faithful, but not dead-scratch-
faithful** at every `dispatch_helper` site. The interpret-floor is *more literal* than the
optimized codegen; it surfaces the optimization, not a bug.

### Proof it is dead / bounded (300-frame attract sweep)

Compiled (bounce) vs interpret-everything, per-frame full-WRAM byte diff + framebuffer
byte diff (`dumpram` + `dumpfb` at each checkpoint):

- **Live WRAM: bit-exact** — 0 diffs outside the two dead zones, every frame.
- **Framebuffer: bit-exact** — 0 / 172086 bytes, every frame.
- The only persistent WRAM diffs are **dead dispatcher scratch `$0000–$0003`** and **dead
  stack residue** (bytes below `S`, e.g. `$01F8–$01FF`), never read before being
  overwritten.
- One transient live byte, `$1DFF LastUsedMusic` (sound-engine bookkeeping; already a
  known recomp-vs-oracle difference, `KOOPA_INVESTIGATION_NOTES.md`), flips by a frame due
  to APU touch-pacing and **reconverges** — not a codegen effect.
- The `ppu` sub-hash differs only because it is `ppu_saveload` of cycle-timing-derived
  internal latches (the interp is not cycle-accurate → the known `pace` divergence);
  **rendered pixels are identical**.

Conclusion: the SMW codegen is **faithful for all live guest state and all rendered
output**. The floor "divergence" is the static-dispatch optimization's dead-scratch gap
plus the interp's non-cycle-accurate pacing — neither a correctness bug.

### How to run the floor gate (mask the known-dead bytes)

Compare live subsystems with the dead zones masked; expect a clean track (live WRAM +
framebuffer bit-exact):

```
--compare ram,ppu,dma --ram-mask "0x0000-0x000F,0x0100-0x01FF"
```

Byte-level / framebuffer proof (more direct than the hash gate, and pace-immune) is the
per-checkpoint `dumpram`+`dumpfb` diff, classifying diffs as live vs dead (`$00–$03` +
stack page). That is the airtight faithfulness check for any game that uses the
`$0086DF`-style dispatch-helper optimization.

### Reproducing the dead scratch in codegen (NOT done — needs sign-off)

Making the synthesized dispatch replay the dispatcher's `A`/`$00–$03` writes would make the
floor literally bit-exact, but it **changes shipped SMW/HLE codegen** (the optimization is
shared with the default HLE build), re-adds dead computation the optimization deliberately
removed, is per-dispatcher-site work, and yields **zero live/visible benefit**. Deferred
pending explicit sign-off; the documented mask is the accepted resolution.

### Dev tooling added for this drill (dev-only, `#ifdef SNES_COSIM`)

- `interp816.c`: always-on per-instruction ring (`pc, op, A-in/out, m/x`), dumped via the
  new cosim server command `itrace <path> [n]`; plus `g_interp816_cur_pc` for the
  `cpu_state.c` write-watch to name an interpreted store. Zero cost in shipped builds
  (`SNES_COSIM` undefined).
