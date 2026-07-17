# snesrecomp — Known Issues (runner / recompiler level)

Game-specific issues live in each game repo's ISSUES.md; this file tracks
issues in the shared runner and recompiler that affect every port.

## ROLLOUT NOTE: widescreen + static-coverage merge (2026-07-16)

Merged from `feat/pr10-widescreen`. Everything is opt-in / pin-gated, but
two things bite each game at its NEXT engine pin bump / regen:

1. **`RendererFuncs` gained a MIDDLE member** (`GetOutputSize`, util.h).
   Positional initializers in game repos mis-bind and fail to compile
   (loud, type mismatch). Fix each game's initializer when it bumps.
2. **Regen output grows.** The analyzer now proves exit-mode SETS, refutes
   poisoned widths, and solves self-recursive exit components (see
   docs/LLE_FIRST_ANALYSIS.md "Static-Coverage Extensions"), so closures
   deepen even without the new `v2_emit --cfg-roots` flag. MMX USA went
   32 → 4,552 exact AOT variants with `--cfg-roots`. Expect bigger gen
   dirs (sharded TUs, LoROM mirror-bank keying) and re-run each game's
   overrides injector after regen. Interp fallbacks are now LOUD at
   runtime (`[tier2] INTERP GAP` + exit summary; SNESRECOMP_TIER2_QUIET=1).

MMX widescreen itself remains WIP and hidden (see MegamanXRecomp
ISSUES.md for the spawn/alignment/savestate ledger).

## OPEN: v2 regeneration mixes fixed-point analysis with full C emission

**Status:** OPEN 2026-07-13. Immediate partial-regeneration link-root fix is
implemented locally and pending cross-game validation. The architectural
redesign below remains future work.

**Observed on ALttP:** a one-bank regeneration repeatedly emitted a roughly
200 MB generated-C program while variant discovery, emit-truth pruning,
reference-taint pruning, and exit-M/X propagation peeled invalid variants one
caller layer at a time. A focused bank-02 run reached seven serial passes at
roughly four minutes per pass. Stopping between passes left bank 02 without
variants still referenced by preserved banks 00 and 1E, producing linker
failures.

**Immediate correctness defect:** partial regeneration used the semantic
reference-taint graph as its link contract. That graph intentionally excludes
alias wrappers, unknown synthetic callers, and runtime M/X dispatch cases,
because those edges must not propagate semantic taint. They are nevertheless
real C linker relocations. Consequently, a variant needed by a preserved bank
could be absent from `skipped_link_targets` and then be pruned from the
regenerated bank. Partial regeneration must scan every generated variant call
in preserved source files, treat those targets as immutable roots, and fail
immediately if a prune intersects that root set.

**Why the current design is slow:** analysis and materialization are coupled.
Any newly discovered or newly pruned variant causes complete bank emission,
then exit-M/X summaries and call routing are recomputed globally. Reference
taint can expose the next invalid caller only after the previous layer has
been removed and re-emitted. The convergence guards measure pass counts, but
alternating prune kinds can reset their streaks and allow pathological runs.

**Required redesign:** make LLE architectural state the analysis ground truth
and make generated AOT/HLE bodies a materialized optimization after that
analysis has stabilized.

1. Build a whole-program variant-demand graph in memory. Preserved-bank link
   targets, vectors, cfg entries, and explicit exports are roots.
2. Propagate entry M/X and callee exit-M/X with a work queue. Revisit only
   functions whose input facts changed.
3. Solve recursive call relationships as strongly connected components so
   recursive width facts converge together instead of peeling one emitted
   caller layer per pass.
4. Run wrong-width and reference-taint pruning on the stabilized graph, not on
   generated C text. A pruned node must never be a root or a dependency of a
   surviving node.
5. Cache decoded functions and exit summaries by ROM/config/compiler-content
   hash. A one-bank change should not re-decode unrelated banks.
6. Emit each affected bank once after analysis reaches a fixed point. Validate
   its exported symbol manifest against all preserved-bank imports before
   invoking the native compiler.
7. Generate into a staging directory and atomically publish only a complete,
   link-contract-valid set. Interrupted regeneration must leave the previous
   generated program usable.
8. Keep an LLE tier-down for any runtime AOT entry whose live architectural
   M/X state has no proven matching body. That is a correctness backstop and
   worklist signal; it must not silently execute a wrong-width AOT variant.

**Success criteria:** focused regeneration has bounded work proportional to
the affected graph, never publishes a cross-bank symbol mismatch, and produces
the same reachable variant set as a clean full regeneration.

## FIX IMPLEMENTED (pending playtest): Music/SFX command can drop under turbo (APU port-write scheduler vs uncapped game clock)

**Status:** FIX IMPLEMENTED 2026-06-17 (pending user playtest). Previously
OPEN/deferred since 2026-06-11. Runner-level — affects all games.

**Escalation (2026-06-17):** users reported the symptom is worse than the
original writeup assumed — audio can drop out ENTIRELY (music AND SFX) at
level transitions and NOT come back, and it occurs occasionally even at
normal speed (turbo just makes it reliably reproducible). Confirmed root
cause is the same same-port command collapse documented below. "Entirely"
= a level transition fires several DISTINCT values at one port (fade,
silence, new song) and a surviving fade can zero global output, taking SFX
down with it; "never comes back" = within a level no further command is
sent, so the documented self-heal-at-next-transition never fires.

**Fix:** per-port minimum dwell in `RtlApuWrite` (`runner/src/common_rtl.c`)
+ `APU_PORT_MIN_DWELL`/larger queue in `runner/src/snes/apu.h`. A DISTINCT
value's scheduled target is floored so the previous distinct value on that
port holds the bus ≥128 produced-samples (~2 engine poll periods) before
being overwritten — guaranteeing the SPC polls every value. Bounded by
produced + 8*quantum so pathological sustained bursts degrade to bounded
latency, never unbounded. No effect at 1x (frame-spaced writes are already
~534 samples apart), so normal-speed scheduling is byte-identical. The
drain runs once per produced sample, so target spacing becomes apply
spacing directly. Pure runner change — no regen; rebuild only.

**Original writeup (mechanism still accurate):**
Observed on Zelda ALttP: overworld music
did not start after entering the overworld while holding Turbo (Tab).
Self-heals at the next music transition. Normal-speed audio is unaffected
(post-fix validation: SMW 100% across two runs, MMX no misses, Zelda clean
at normal speed).

**Symptom:** while turboing, a one-shot music command written to an APU port
can be silently lost — area music stays silent until the next track change.
One-shot SFX can drop under turbo too (inaudible in practice at 5-10x speed).

**Mechanism (understood, not a regression):** since `bf64f0d` the runner
schedules CPU APU-port writes in APU-sample time (write-clock targets spaced
by the wall-time gap between writes; floor = produced clock, ceiling =
produced + 3 audio-callback quanta). Turbo runs the emulated game uncapped
while the audio device keeps consuming at real time, so port writes arrive at
5-10x wall rate against an APU advancing at 1x. The write stream compresses
against the latency ceiling; back-to-back writes to the same port can apply
with near-zero engine time between them, and the SPC engine (polling every
~64 samples of its own time) never observes the overwritten value. Pre-fix
behavior was equally lossy under turbo (wall-time port mutation gave a
command microseconds of APU time); the scheduler makes the loss bounded and
characterizable.

**Evidence path if it recurs:** keep the process alive and query the
always-on port rings (`audio_events filter=2` on the debug server, or SMW's
`tools/sfx_probe.py chain` pointed at the game's debug port — SMW 4377,
Zelda 4378, MMX 4379) — every command's fate (SEEN / LOST, with apply
spacing) is in the ring.

**Proposed hardening (when picked up):** in `RtlApuWrite`
(`runner/src/common_rtl.c`), when the latency ceiling is clamping (turbo
pressure), enforce a minimum ~2-engine-tick spacing (~128 samples) between
DISTINCT values applied to the same port and drop middle values of a burst
instead of compressing all spacing to zero — the engine then reliably sees
the last command of every burst, which is the one that matters for music.
Keep total latency bounded; do not stretch the wall clock (the MMX issue-4
"never bound catch-up to real time" rule still applies).
