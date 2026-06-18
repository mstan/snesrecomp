/*
 * interp_bridge.c — interp816 <-> AOT bridge. See interp_bridge.h and
 * docs/MULTI_TIER.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"
#include "interp816.h"

/* ── memory bus shim ───────────────────────────────────────────────────────
 * The interpreter's `mem` is the CpuState*; route every access through the
 * same AOT HLE bus the compiled code uses, so the interpreter sees identical
 * WRAM / MMIO / SRAM / ROM. One memory map, zero divergence. */
static uint8_t bridge_bus_read(void *mem, uint32_t adr) {
    CpuState *cpu = (CpuState *)mem;
    return cpu_read8(cpu, (uint8)((adr >> 16) & 0xFF), (uint16)(adr & 0xFFFF));
}
static void bridge_bus_write(void *mem, uint32_t adr, uint8_t val) {
    CpuState *cpu = (CpuState *)mem;
    cpu_write8(cpu, (uint8)((adr >> 16) & 0xFF), (uint16)(adr & 0xFFFF), val);
}

/* ── register/flag sync ────────────────────────────────────────────────────
 * interp816 carries flags as discrete bools + an `e` (emulation) bit; CpuState
 * carries packed P + per-bit mirrors + m/x/emulation. Map both directions.
 * PC has no CpuState home (control flow is host-C calls) — it is interp-only
 * and set explicitly by the run loop, never synced. */
static void sync_cpu_to_interp(const CpuState *c, Interp816 *in) {
    in->a  = c->A;  in->x = c->X;  in->y = c->Y;
    in->sp = c->S;  in->dp = c->D; in->db = c->DB; in->k = c->PB;
    in->c  = c->_flag_C; in->z = c->_flag_Z; in->v = c->_flag_V;
    in->n  = c->_flag_N; in->i = c->_flag_I; in->d = c->_flag_D;
    in->mf = c->m_flag;  in->xf = c->x_flag; in->e = c->emulation;
}
static void sync_interp_to_cpu(const Interp816 *in, CpuState *c) {
    c->A = in->a;  c->X = in->x;  c->Y = in->y;
    c->S = in->sp; c->D = in->dp; c->DB = in->db; c->PB = in->k;
    c->_flag_C = in->c; c->_flag_Z = in->z; c->_flag_V = in->v;
    c->_flag_N = in->n; c->_flag_I = in->i; c->_flag_D = in->d;
    c->m_flag  = in->mf; c->x_flag = in->xf; c->emulation = in->e;
    cpu_mirrors_to_p(c);   /* keep packed P consistent for PHP/PLP/stack ops */
}

/* BRK bridge seam. The bounce is via explicit JSR/JSL interception below, not
 * via planted BRKs, so an interpreted BRK is treated as a no-op continue.
 * (Production hardening may route an unexpected BRK to a contained stop.) */
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

/* Safety cap: a coverage gap must never wedge the host in an unbounded loop.
 * A real self-contained routine is thousands–tens-of-thousands of steps; a
 * bail means the interpreted routine didn't terminate (an infinite loop —
 * e.g. a garbage indirect target from upstream-corrupted state, as the
 * $0FE8B7 / JMP ($0012)=$FFFF investigation showed). Default 2M is well clear
 * of any real routine while keeping a bail's freeze short; tunable via
 * SNESRECOMP_INTERP_STEP_CAP. (A proper fix detects the tight repeating-PC
 * loop and bails early — future work.) */
static long interp_step_cap(void) {
    static long v = 0;
    if (v == 0) {
        const char *e = getenv("SNESRECOMP_INTERP_STEP_CAP");
        v = e ? atol(e) : 2000000L;
        if (v < 1000) v = 1000;
    }
    return v;
}

/* Opt-in diagnostic (SNESRECOMP_INTERP_TRACE=1): on a step-cap BAIL, dump the
 * entry path + the loop the interpreter was stuck in, so we can classify a
 * bail as hardware-wait spin vs wrong-target vs mis-decode. Off by default. */
typedef struct { uint32_t pc; uint8_t op; } ITraceEnt;
static int itrace_enabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("SNESRECOMP_INTERP_TRACE") ? 1 : 0;
    return v;
}
static void itrace_dump(uint32_t entry, const ITraceEnt *head, int nhead,
                        const ITraceEnt *ring, long total) {
    fprintf(stderr, "[interp_trace] BAIL entry=$%06X total_steps=%ld\n",
            entry, total);
    fprintf(stderr, "[interp_trace] entry path:\n");
    for (int i = 0; i < nhead; i++)
        fprintf(stderr, "    [%d] $%06X op=$%02X\n", i, head[i].pc, head[i].op);
    /* Last 48 steps = the loop it is stuck in. */
    long start = total > 48 ? total - 48 : 0;
    fprintf(stderr, "[interp_trace] last %ld steps (the spin):\n", total - start);
    for (long i = start; i < total; i++) {
        const ITraceEnt *e = &ring[i & 255];
        fprintf(stderr, "    $%06X op=$%02X\n", e->pc, e->op);
    }
}

/* Core: interpret from entry_pc24 until an RTS/RTL leaves cpu->S strictly
 * above `s_exit` (the routine returned to its caller). `s_exit` is the FRAME
 * BASE to unwind to — for a tail-dispatch / PEA+JMP re-interpret it is the
 * enclosing function's _entry_s, NOT the current cpu->S (a PEA may have pushed
 * a return below entry, and the target's RTS-to-PEA must NOT end the bridge). */
static int interp_bridge_run_ex(CpuState *cpu, uint32_t entry_pc24,
                                uint16_t s_exit) {
    /* Local interpreter context → nesting (an AOT bounce that itself traps and
     * re-enters the bridge) gets its own frame; no shared mutable interp. */
    Interp816 in;
    memset(&in, 0, sizeof in);
    in.mem = cpu;
    in.read = bridge_bus_read;
    in.write = bridge_bus_write;

    sync_cpu_to_interp(cpu, &in);
    in.k  = (uint8)((entry_pc24 >> 16) & 0xFF);
    in.pc = (uint16)(entry_pc24 & 0xFFFF);

    /* Frame base: the routine has returned to its caller when an RTS/RTL pops
     * cpu->S strictly above this. */
    const uint16_t s_enter = s_exit;

    const int trace = itrace_enabled();
    ITraceEnt head[8], ring[256];
    long itn = 0;

    const long step_cap = interp_step_cap();
    for (long steps = 0; steps < step_cap; steps++) {
        const uint32_t pc_before = ((uint32_t)in.k << 16) | in.pc;
        const uint8_t  op = bridge_bus_read(cpu, pc_before);
        if (trace) {
            ITraceEnt _e = { pc_before, op };
            if (itn < 8) head[itn] = _e;
            ring[itn & 255] = _e;
            itn++;
        }

        /* Subroutine calls: JSR abs (0x20, 3B), JSL (0x22, 4B),
         * JSR (abs,X) (0xFC, 3B). RTS (0x60) / RTL (0x6B) are returns. */
        const int is_call  = (op == 0x20 || op == 0x22 || op == 0xFC);
        const int call_len = (op == 0x22) ? 4 : 3;
        const int is_ret   = (op == 0x60 || op == 0x6B);

        interp816_runOpcode(&in);   /* executes the opcode; pushes/pops frames */

        if (is_call) {
            /* The interp just pushed the real hardware return frame (return-1)
             * and set pc to the target. If the target has a compiled body for
             * the current (m,x), run it compiled. */
            sync_interp_to_cpu(&in, cpu);          /* expose (m,x) + frame to AOT */
            const uint32_t target = ((uint32_t)in.k << 16) | in.pc;
            if (cpu_dispatch_has_entry(cpu, target)) {
                /* cpu_dispatch_pc runs the variant with host_return_valid=0;
                 * its RTS pops the frame the interp pushed and dispatch-misses
                 * on the return addr → S restored to pre-call. Balanced. */
                cpu_dispatch_pc(cpu, target, cpu->S);
                sync_cpu_to_interp(cpu, &in);
                const uint32_t ret = (pc_before + (uint32_t)call_len) & 0xFFFFFF;
                in.k  = (uint8)((ret >> 16) & 0xFF);
                in.pc = (uint16)(ret & 0xFFFF);
            }
            /* else: no compiled body → keep interpreting into the target
             * (this is the coverage-gap path; Phase 1b records it). */
            continue;
        }

        if (is_ret && (uint16_t)in.sp > s_enter) {
            /* The interpreted routine returned past its entry depth. */
            sync_interp_to_cpu(&in, cpu);
            return 1;
        }
    }

    /* Step cap hit — contained bail. Sync so observable state is consistent;
     * the caller treats a 0 return as "gap not cleanly resolved". */
    if (trace) itrace_dump(entry_pc24, head, (int)(itn < 8 ? itn : 8), ring, itn);
    sync_interp_to_cpu(&in, cpu);
    return 0;
}

/* Public entry: exit watermark = the current stack depth (the routine is
 * entered balanced at cpu->S). */
int interp_bridge_run(CpuState *cpu, uint32_t entry_pc24) {
    return interp_bridge_run_ex(cpu, entry_pc24, cpu->S);
}

/* ── tier-down entry (called from generated indirect-dispatch defaults) ───── */

static long s_tier_hits = 0;
long interp_tier_hit_count(void) { return s_tier_hits; }

/* Bounded observability: a coverage-gap tier-down is an event worth seeing.
 * First N go to stderr (matching the existing dispatch_oob single-line style);
 * the counter is always live for the manifest (Phase 2) and tests. */
static void interp_tier_note(uint32_t target_pc24) {
    long n = ++s_tier_hits;
    if (n <= 32)
        fprintf(stderr, "[interp_tier] #%ld -> $%06X\n", n,
                (unsigned)(target_pc24 & 0xFFFFFF));
}

RecompReturn interp_tier_dispatch(CpuState *cpu, uint32_t target_pc24) {
    interp_tier_note(target_pc24);
    /* Interpret the routine the static pass couldn't resolve. It shares cpu's
     * stack, so its RTS/RTL pops the inherited caller frame and the bridge
     * exits past entry; control then unwinds to the dispatcher's caller, same
     * as an AOT tail-dispatch would. (Bail -> still NORMAL: contained, the
     * caller continues; a wedged gap is a bug to surface, not to hang on.) */
    (void)interp_bridge_run(cpu, target_pc24 & 0xFFFFFF);
    return RECOMP_RETURN_NORMAL;
}

/* Upgrade of an unresolved tail-dispatch site (one that would otherwise call
 * cpu_unresolved_abandon_balanced): run the target instead of dropping it. On
 * a clean return the routine's RTS/RTL has balanced the stack; on a bail fall
 * back to the stack-safe abandon so we are never worse than the drop path. */
RecompReturn interp_tier_dispatch_balanced(CpuState *cpu, uint32_t target_pc24,
                                           uint32_t site_pc24, uint16_t entry_s,
                                           uint8_t hrv) {
    interp_tier_note(target_pc24);
    /* Unwind watermark is the enclosing function's entry_s (NOT the current S:
     * a PEA+JMP idiom may have pushed a return below entry). Exit when the
     * function RTS/RTLs past entry_s. */
    if (interp_bridge_run_ex(cpu, target_pc24 & 0xFFFFFF, entry_s))
        return RECOMP_RETURN_NORMAL;
    return cpu_unresolved_abandon_balanced(cpu, site_pc24, entry_s, hrv);
}
