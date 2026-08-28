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

/* SA-1's frame timeline already advances the SPC to absolute guest time, so
 * its interpreter must not also apply the legacy relative catch-up. Ordinary
 * SNES cartridges still require that catch-up during interpreter-heavy boot
 * code (Mega Man X otherwise stalls in task 0 at the copyright screen). */
static inline bool interp_bridge_use_absolute_apu_timeline(
    bool frame_timeline_active, bool is_sa1) {
  return frame_timeline_active && is_sa1;
}

/* Optional game policy invoked immediately before one interpreted opcode.
 * The bridge compares the live PC first, so ordinary interpreted instructions
 * pay only the address check. At a match it synchronizes registers into
 * CpuState, invokes the callback, then copies any changes back.
 *
 * Up to 8 PCs may be armed (LoROM FastROM bit7 is masked). Calling with a
 * non-NULL hook adds/replaces that PC; hook=NULL clears all slots. */
typedef void (*InterpPreOpcodeHook)(CpuState *cpu, uint32_t pc24);
void interp_bridge_set_pre_opcode_hook(uint32_t pc24,
                                       InterpPreOpcodeHook hook);
void interp_bridge_pre_opcode_redirect(uint32_t pc24);
/* Dump the last n entries of the always-on global interp step ring
 * (pc/op/sp/frame per interpreted opcode) to `out` (NULL = stderr). */
#include <stdio.h>
void interp_bridge_dump_recent_steps(int n, FILE *out);

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
 * waiting (Super Metroid), rather than cleared after a slot walk (MMX).
 *
 * This is not a CPU-only helper: interpreted opcodes advance the shared Snes
 * beam through snes_sync_master_clock(). Frame-model hosts that also own a
 * beam loop must integrate with that shared clock or manually reproduce the
 * framework edges listed in docs/FRAME_MODEL_HOSTS.md. */
int interp_bridge_run_loop(CpuState *cpu, uint32_t entry_pc24,
                           uint32_t yield_pc, uint16_t flag_addr,
                           uint8_t flag_value);

/* Run a whole-program LLE continuation until it reaches a deterministic
 * read-only cycle: the complete architectural state repeats without any bus
 * write.  Such a cycle can only make progress through asynchronous hardware
 * (NMI/IRQ/coprocessor), so it is a general frame boundary rather than a
 * game-address hint. */
int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24);
uint32_t interp_bridge_lle_resume_pc(void);

/* True if the most recent auto-quiescent yield was a 65816 WAI. The host
 * should deliver NMI/IRQ before resuming at interp_bridge_lle_resume_pc().
 * Sticky until read (then cleared). */
int interp_bridge_lle_took_wai(void);

/* Optional whole-program LLE deadline.  When nonzero, the auto-quiescent
 * bridge yields at the first architectural instruction boundary whose master
 * clock reaches this value.  Event-driven game schedulers use this to prevent
 * a productive CPU/MMIO loop from running across multiple vblanks atomically.
 * Combine with snes_next_irq_master() when raster IRQs can occur before the
 * next frame boundary. */
void interp_bridge_set_master_deadline(uint64_t master_clock);

/* True only while a paired AOT bounce is executing inside an auto-quiescent
 * scheduler whose current frame deadline has been reached. Long,
 * architecturally interruptible instructions use this at their legal byte
 * boundaries before unwinding to the owning interpreter. */
int interp_bridge_lle_master_deadline_reached(const CpuState *cpu);

/* Execute an architectural interrupt handler through its terminal RTI. The
 * caller has already materialized the hardware interrupt frame, usually with
 * cpu_push_interrupt_frame_at(). Do not enter an interrupt body directly from a
 * host scheduler unless that frame is on the guest stack for the terminal RTI
 * to consume. */
int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24);

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
void interp_tier2_stats(int *sites, unsigned long long *clean,
                        unsigned long long *bail);

/* ── Phase-2 gap manifest (always-on coverage worklist) ────────────────────
 * Every tier-down is recorded into a growable in-memory table keyed by
 * (site, target, m/x width, kind), tracking clean-return vs contained-bail counts
 * and the frame span. This is the WORKLIST the offline ingest tool
 * (tools/tier2_ingest.py, Phase 3) folds back into the cfg so the next regen
 * makes the discovered entries Tier-1 AOT. Recording is cheap and lives in
 * every config (Production included) — it is NOT gated behind SNESRECOMP_TRACE.
 *
 * Each tuple's first sighting is immediately appended and flushed to a JSONL
 * journal. A unique final manifest is written on normal exit; the journal is
 * the recovery source if the process cannot run its exit handlers.
 *
 * Tier2CoverageDumpJson embeds the table into the unified post-mortem report
 * (build/last_run_report.json), with a trailing comma like the other
 * dump_*_json sections. Tier2CoverageWriteManifest writes the slim standalone
 * manifest (schema "snesrecomp tier2 coverage v1") that the ingest tool reads. */
void Tier2CoverageDumpJson(FILE *f);
void Tier2CoverageWriteManifest(const char *path, const char *rom_title);
void Tier2CoverageWriteDefaultManifest(const char *rom_title);
#ifdef SNESRECOMP_TIER2_TEST
void Tier2CoverageTestRecord(uint32_t site, uint32_t target, uint8_t mx,
                             uint8_t kind, int clean);
#endif

#endif /* INTERP_BRIDGE_H */
