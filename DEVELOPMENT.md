# snesrecomp — Development Log (branch `investigate/sm-0012-blocker`)

Recompiler-side development record for the Super Metroid (game #4) bring-up.
The full bring-up narrative lives in `SuperMetroidRecomp/DEVELOPMENT.md`; this
file tracks the **engine** changes and the recompiler-side design decisions.

> Discipline: fix the generator (`recompiler/v2/*.py`) or the C runtime
> (`runner/src/`), never the generated `src/gen/`. No stubs. Full-regen before
> building (partial `--banks` breaks cross-bank variant refs). Prefer the
> general/complete fix over a narrow patch.

---

## Context: SM attract-demo bring-up

The SM attract demo was pinned at `game_state` 40. **Root cause was a
suppressed indirect call**, not the earlier "DB-divergence" theory. The fix was
a game-repo cfg directive (`SuperMetroidRecomp/recomp/bank02.cfg`):
`indirect_dispatch 817b 5 ptrcall targets:891A,8924,8925,892B,8932` —
authorizing `JSR ($0012,X)` at `$82:817B` so the dropped `INC $0998`
(`++game_state`) at `$82:817E` runs again. Verified via recomp-vs-oracle
game_state timeline diff (40→41→42, abandons=0). No engine change was needed
for that fix.

The crash then advanced to a `WriteEnemyOams` infinite loop at the first
demo-gameplay frame — **this one needs an engine fix.**

---

## The suppressed-indirect-call class (engine design)

### Current behavior (`cfg-required-dispatch-or-kill`, 2026-05-03)
`recompiler/v2/decoder.py` (~line 1998): a `JSR (abs,X)` is only emitted as a
real dispatch when authorized via `indirect_dispatch` (unified) /
`indirect_call_table` (legacy), or auto-recovered as a same-bank ROM table
(`_autorecover_indirect_xtable`). Otherwise it is **UNAUTHORISED**: the decoder
drops the fall-through edge (line ~2124), records a `SuppressedIndirectCall`,
and `recompiler/v2/codegen.py::_emit_call` (~line 1811) emits only a comment.
At runtime the function **silently returns `RECOMP_RETURN_NORMAL`** — no record,
no abandon.

This was deliberate (phantom-SMC suppression: garbage bytes past an RTS can
decode as a phantom `JSR (abs,X)`; see
`tests/v2/test_decoder_smc_phantom_suppression.py`). But it produces
**false negatives** for real, reachable runtime-pointer dispatches.

### Why it bites SM
SM's enemy/PLM/eproj instruction-list interpreters dispatch via
`JSR ($0FA8/$0FAE/$0FB0/$0FB2,X)` in banks `$22-$2A`. `$0FAx` are **WRAM**
per-object function pointers (`$22:0FAx` mirrors `$7E:0FAx`; `JSR (abs,X)` reads
the target from `PB:(operand+X)`). These run each object's init + AI. Suppressed
→ enemy fields never initialize → `WriteEnemyOams` walks a garbage spritemap
count and loops forever. The targets are runtime WRAM pointers — they **cannot
be statically enumerated**, so the enumerated `ptrcall targets:…` form does not
apply.

### Existing runtime machinery (already present — reuse it)
`runner/src/cpu_state.c`:
- **`cpu_dispatch_pc_from(cpu, pc24, entry_s_for_miss_restore, source_pc24)`** —
  true runtime indirect call: binary-search `g_dispatch_table`, call the correct
  `(m,x)` variant, LoROM bank-mirror fallback, controlled `S`-restore + `NORMAL`
  on miss.
- **`g_dispatch_log`** — always-on 1024-entry ring of every dispatch
  `(pc24, source_pc24, func_name, mx_idx, found, mirror, frame)`; read via
  `cpu_dispatch_log_count()` / `cpu_dispatch_log_at(i)`.

### Planned fix (next session)
Route reachable runtime-pointer indirect calls through `cpu_dispatch_pc_from`:
1. **Decoder:** for a reachable `JSR (abs,X)` with a WRAM/DP pointer base
   (operand `< $2000`) and no static table, **preserve the fall-through** and
   mark a new `dispatch_runtime` form (no enumerated entries). Keep phantom
   suppression for non-WRAM / garbage operands so
   `test_decoder_smc_phantom_suppression` stays green.
2. **Codegen:** read the pointer word from WRAM `operand+X` at runtime and call
   `cpu_dispatch_pc_from(cpu, (bank<<16)|ptr, _entry_s, site_pc24)`. Fall through
   on return; clean unwind on miss; auto-logged in `g_dispatch_log`.

Preferred over per-site cfg (`indirect_dispatch <site> runtime ptrcall`, ~111
lines) per the "most complete option" rule — the auto-policy covers every
present and future site.

**Also planned (observability):** dump `g_dispatch_log` into
`build/last_run_report.json` (the TCP debug server is unusable for SM — it dies
in ~30s — so the post-mortem report is the only readable always-on ring).

---

## SHIPPED (2026-06-22): runtime-pointer dispatch + interp-tier paired-ABI fix

Both the planned runtime-dispatch fix above AND a second, deeper engine fix
landed and are VERIFIED (SM attract demo runs past the old f2689 freeze to
f4086+, no crash). Runtime-only; NO regen for the second fix.

1. **Runtime-pointer `JSR (abs,X)` dispatch** (as planned): decoder marks
   `dispatch_runtime` for reachable WRAM-operand (`< $2000`) sites, codegen
   `_emit_runtime_dispatch` reads the live pointer + calls `cpu_dispatch_call_pc`
   (cpu_state.c) — paired host-call to the AOT body, else interpreter tier
   (`interp_tier_run_call`, interp_bridge.c). Suppressed-dispatch count 111→9.
   New test `tests/v2/test_decoder_runtime_dispatch.py`. Observability:
   `CpuDispatchLogDumpJson` (g_dispatch_log → report).

2. **Interp-tier AOT-bounce ABI bug** (the actual f2689 root cause): the bridge
   (`interp_bridge_run_ex`) bounced AOT sub-calls via `cpu_dispatch_pc`
   (dispatch ABI, hrv=0), whose callee RTS **re-dispatches on the popped return
   address**. When that address is itself a registered function entry (e.g.
   `$90:EB55 sub_90EB55` right after `HandleChargingBeamGfxAudio`'s JSR, reached
   via SamusDrawSprites's `$90:EB4E` unresolved-IndirectGoto tail dispatch), the
   dispatch HITS it and runs the next routine as part of the callee's "return"
   → over-pops `cpu->S` by the frame size (+2) → `DB=$74` leak → WriteEnemyOams
   freeze. FIX = `cpu_dispatch_pc_paired(cpu, pc24, frame_size)` (cpu_state.c):
   run the target with `host_return_valid=frame_size` so it HOST-RETURNS to the
   bridge instead of re-dispatching; the bridge AOT-bounce now uses it. General
   fix for ALL interp-tier AOT-bounces.

   Diagnostic probes added (env-gated, default off): `SNESRECOMP_SBOUND=lo-hi`
   (cpu->S+DB at every block in a pc24 range — cpu_trace.c), `SNESRECOMP_IBRWATCH=lo-hi`
   (per-step interp-bridge trace — interp_bridge.c). GOTCHA: the stack-balance
   auditor (`stack_balance` in the report) counts the return-frame pop, so a
   balanced JSR reads as +2 / JSL as +3 — NOT a true-leak signal; use the
   S-boundary probe to localize a real imbalance.

Full SM-side writeup: `SuperMetroidRecomp/DEVELOPMENT.md` (2026-06-22 milestone).

---

## Recent engine commits on this branch
- `1b3201d` block-boundary DB shadow + DB-write log (catch inline PLBs)
- `5bc8bf3` env-gated DB-trace at the function-entry hook
- `78fba4e` multi-tier: gap manifest (P2) + ingest (P3) + bank-miss tier-down (P4)

## Open / owner-gated
Reconciling the two divergent multi-tier base branches
(`feat/multi-tier-interp-fallback` vs `integ/sm-interp`) is owner-gated.
Merging to main / releasing is owner-gated.
