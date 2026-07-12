/*
 * interp_bridge — the interp816 <-> AOT bridge (interpreter-fallback tier).
 *
 * Entered at a trap site (Phase 1b: dispatch_oob / bank-miss; the spike uses
 * a synthetic site) with a known guest PC and the live CpuState. Runs the
 * LakeSnes-derived interpreter (interp816) over guest code, SHARING the
 * caller's register state and memory:
 *   - memory goes through the AOT cpu_read8 / cpu_write8 HLE bus (one map);
 *   - register/flag state is synced CpuState <-> Interp816 at every crossing.
 *
 * When the interpreted code calls (JSR/JSL) into a guest address that has a
 * compiled body (a g_dispatch_table entry for the current (m,x)), the call is
 * BOUNCED through cpu_dispatch_pc so compiled code keeps running compiled;
 * the interpreter resumes at the return address. Honors the Option-1 cpu->S
 * return-frame model (see cpu_state.h / docs/MULTI_TIER.md).
 *
 * The bridge exits when the interpreted routine returns past its entry stack
 * depth (an RTS/RTL that leaves cpu->S above the value at entry). The caller
 * must have a return frame (or sentinel) on cpu->S so that final RTS has
 * something to pop — exactly as the Option-1 model already arranges for
 * dispatched entries.
 *
 * Anti-RECURSION_BUG contract (docs/MULTI_TIER.md §6): re-entry is bounded by
 * guest call depth (bounces RETURN, never stack a permanent interp context),
 * exit asserts a balanced stack, and there is no host recursion unmatched by
 * a guest return.
 */
#ifndef INTERP_BRIDGE_H
#define INTERP_BRIDGE_H

#include <stdint.h>
#include <stdio.h>
#include "cpu_state.h"

/*
 * Run the interpreter over guest code at entry_pc24, in the context of `cpu`.
 * `cpu` is updated in place. Returns:
 *   1 = the routine returned cleanly (balanced past entry);
 *   0 = the bridge bailed (iteration cap / contained failure).
 */
int interp_bridge_run(CpuState *cpu, uint32_t entry_pc24);

/* Faithful LLE of an infinite cooperative-scheduler loop (e.g. MMX's $8099 task
 * scheduler): run the real ROM scheduler under interp816 from entry_pc24 and
 * yield after one frame's slot walk — when it reaches yield_pc (its vblank-wait
 * spin) with the flag byte at flag_addr cleared. Tasks it dispatches bounce to
 * compiled bodies via the paired ABI. Returns 1 on clean yield, 0 on cap bail. */
int interp_bridge_run_scheduler(CpuState *cpu, uint32_t entry_pc24,
                                uint32_t yield_pc, uint16_t flag_addr);

/* General infinite-loop driver.  This is the scheduler helper with an
 * explicit byte value for games whose vblank wait flag is asserted while
 * waiting (Super Metroid), rather than cleared after a slot walk (MMX). */
int interp_bridge_run_loop(CpuState *cpu, uint32_t entry_pc24,
                           uint32_t yield_pc, uint16_t flag_addr,
                           uint8_t flag_value);

/* Save-state task resume: interpret a suspended cooperative task from its
 * recorded yield return address (an arbitrary mid-function guest PC; the
 * caller restores the task's CpuState first). Calls bounce to compiled bodies
 * via the paired ABI — including yield HLEs, which suspend the hosting fiber
 * exactly like the compiled path — so after one interpreted function frame the
 * task runs mostly compiled again. Returns 1 when the task's top-level RTS
 * unwinds past task_base_s (task finished), 0 on a step-cap wedge bail. The
 * step cap resets on every successful bounce (it bounds interp-side wedges,
 * not the resumed task's lifetime). */
int interp_bridge_resume_task(CpuState *cpu, uint32_t resume_pc24,
                              uint16_t task_base_s,
                              const uint32_t *stop_pcs, int n_stop);

/* Production tier-down entry, called from generated indirect-dispatch defaults
 * (an absolute-indirect JMP/JML whose loaded target isn't in the static case
 * list). Interprets the target instead of silently dropping the transfer;
 * always returns RECOMP_RETURN_NORMAL. Declared in cpu_state.h too (so
 * generated code sees it without including this header). */

/* Count of tier-downs taken this run (observability / tests / Phase-2
 * manifest). */
long interp_tier_hit_count(void);

/* ── Phase-2 gap manifest (always-on coverage worklist) ────────────────────
 * Every tier-down is recorded into a bounded in-memory table keyed by
 * (site, target, m/x width), tracking clean-return vs contained-bail counts
 * and the frame span. This is the WORKLIST the offline ingest tool
 * (tools/tier2_ingest.py, Phase 3) folds back into the cfg so the next regen
 * makes the discovered entries Tier-1 AOT. Recording is cheap and lives in
 * every config (Production included) — it is NOT gated behind SNESRECOMP_TRACE.
 *
 * Tier2CoverageDumpJson embeds the table into the unified post-mortem report
 * (build/last_run_report.json), with a trailing comma like the other
 * dump_*_json sections. Tier2CoverageWriteManifest writes the slim standalone
 * manifest (schema "snesrecomp tier2 coverage v1") that the ingest tool reads. */
void Tier2CoverageDumpJson(FILE *f);
void Tier2CoverageWriteManifest(const char *path, const char *rom_title);

#endif /* INTERP_BRIDGE_H */
