/*
 * interp_bridge.c — interp816 <-> AOT bridge. See interp_bridge.h and
 * docs/MULTI_TIER.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"
#include "interp816.h"
#include "snes.h"   /* Snes, apuCatchupCycles, snes_catchupApu */
#include "cosim.h"  /* cosim_insn — instruction-granular lockstep (no-op unless SNES_COSIM) */

/* Guest-time-anchored APU (Rockman X JP gate #3 / audio pacing): the interp
 * tier advances the SPC per interpreted opcode by guest master cycles, exactly
 * like the faithful reference (cosim/ref_driver.c). Cold-boot handshakes that
 * poll an APU port ($2140 == $AABB) can only complete once the SPC has actually
 * run; the recomp's compiled steady-state paces the SPC on APU-port touches +
 * the wall-clock audio thread, but interpreted boot code hits neither before the
 * poll, so the SPC stayed frozen (co-sim: A outPorts=0000 vs B outPorts=AABB).
 * Scoped to the interp tier — the compiled path never enters here. */
extern Snes *g_snes;
extern uint64_t g_apu_last_sync_master;   /* common_rtl.c — keep synced so a bounce's accurate-mode delta excludes interp opcodes */
extern int g_interp_apu_driving;          /* common_rtl.c — suppresses the per-touch synthetic catch-up while set */
#ifdef SNES_COSIM
extern int cosim_apu_shared_clock(void);  /* common_rtl.c — SNES_COSIM_APU_SHARED touch-only APU pacing */
#endif
void RtlApuLock(void);                    /* real mutex in the windowed runner (audio thread also cycles the SPC); no-op in headless/cosim */
void RtlApuUnlock(void);
static const double kInterpApuPerMaster = (32040.0 * 32.0) / (1364.0 * 262.0 * 60.0);

/* Batched guest-time APU advance. v1 advanced the SPC per interpreted opcode:
 * RtlApuLock + snes_catchupApu once per opcode. In the windowed build that
 * mutex is contended by the audio thread's bulk SPC bursts, and the per-opcode
 * acquire/catchup collapsed interp-heavy frames ~250x (USA rich-cfg LLE live:
 * 0.25 fps vs 63 fps with audio off — the "chug"). Batch instead: accumulate
 * master cycles locally and convert at (a) any APU-port bus access — BEFORE
 * the access, so every port read/write still sees the SPC exactly as current
 * as the per-opcode scheme gave it, (b) every ~4096 master cycles (~190 SPC
 * cycles, well under one output sample quantum), (c) every bridge exit and
 * AOT bounce. Game-thread only (like the interp itself); nesting shares the
 * accumulator safely because flushing early is always correct. */
static uint64_t s_apu_pending_master = 0;
static void bridge_apu_flush(CpuState *cpu) {
    if (!s_apu_pending_master) return;
    RtlApuLock();
    g_snes->apuCatchupCycles += (double)s_apu_pending_master * kInterpApuPerMaster;
    g_apu_last_sync_master = cpu->master_cycles;
    snes_catchupApu(g_snes);
    RtlApuUnlock();
    s_apu_pending_master = 0;
}
static int bridge_is_apu_port(uint32_t adr) {
    uint16_t a = (uint16_t)(adr & 0xFFFF);
    if (a < 0x2140 || a > 0x217F) return 0;
    uint8_t bank = (uint8_t)((adr >> 16) & 0xFF);
    return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
}

/* ── memory bus shim ───────────────────────────────────────────────────────
 * The interpreter's `mem` is the CpuState*; route every access through the
 * same AOT HLE bus the compiled code uses, so the interpreter sees identical
 * WRAM / MMIO / SRAM / ROM. One memory map, zero divergence. */
static uint8_t bridge_bus_read(void *mem, uint32_t adr) {
    CpuState *cpu = (CpuState *)mem;
    if (bridge_is_apu_port(adr)) bridge_apu_flush(cpu);
    return cpu_read8(cpu, (uint8)((adr >> 16) & 0xFF), (uint16)(adr & 0xFFFF));
}
static void bridge_bus_write(void *mem, uint32_t adr, uint8_t val) {
    CpuState *cpu = (CpuState *)mem;
    if (bridge_is_apu_port(adr)) bridge_apu_flush(cpu);
    cpu_write8(cpu, (uint8)((adr >> 16) & 0xFF), (uint16)(adr & 0xFFFF), val);
}

/* Word bus (interp816 read_word/write_word): claim a CONTIGUOUS pair that
 * lands in the HW-register window and perform it through the same
 * width-preserving AOT bus the compiled code uses (cpu_read16/cpu_write16 →
 * ReadRegWord/WriteRegWord). Root cause this closes: a guest 16-bit store to
 * $2140 (kick lo, data hi) executed as two byte writes releases the APU lock
 * between the bytes, so the audio thread can run the SPC hundreds of samples
 * with the kick applied but the data stale — the driver latches garbage and
 * the upload/command handshake wedges (the USA rich-cfg LLE live wedge/garble;
 * nondeterministic because it races the callback). On silicon the two bus
 * cycles sit inside one SPC cycle — atomic. WriteRegWord's hi-then-lo APU
 * order restores that atomicity; ReadRegWord's snapshot likewise fixes torn
 * 16-bit $2140 polls. Non-HW / wrapping / RMW-reversed pairs fall back to the
 * exact byte-pair behavior. */
static int bridge_hw_word(uint32_t adrl, uint32_t adrh) {
    if (adrh != adrl + 1) return 0;               /* contiguous, no wrap */
    uint16_t a = (uint16_t)(adrl & 0xFFFF);
    if (a < 0x2000 || a + 1 >= 0x6000) return 0;  /* both bytes in HW window */
    uint8_t bank = (uint8_t)((adrl >> 16) & 0xFF);
    return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
}
static bool bridge_bus_read_word(void *mem, uint32_t adrl, uint32_t adrh,
                                 uint16_t *out) {
    if (!bridge_hw_word(adrl, adrh)) return false;
    CpuState *cpu = (CpuState *)mem;
    if (bridge_is_apu_port(adrl)) bridge_apu_flush(cpu);
    *out = cpu_read16(cpu, (uint8)((adrl >> 16) & 0xFF), (uint16)(adrl & 0xFFFF));
    return true;
}
static bool bridge_bus_write_word(void *mem, uint32_t adrl, uint32_t adrh,
                                  uint16_t val, bool reversed) {
    /* reversed = RMW write-back (high byte first on hardware); keep those on
     * the faithful byte path — WriteRegWord would flip non-APU order. */
    if (reversed || !bridge_hw_word(adrl, adrh)) return false;
    CpuState *cpu = (CpuState *)mem;
    if (bridge_is_apu_port(adrl)) bridge_apu_flush(cpu);
    cpu_write16(cpu, (uint8)((adrl >> 16) & 0xFF), (uint16)(adrl & 0xFFFF), val);
    return true;
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
/* yield_pc != 0 selects "cooperative-loop" mode: the interpreted routine is an
 * infinite loop (e.g. MMX's $8099 task scheduler) that never returns — it only
 * yields when it reaches yield_pc with the vblank flag at yield_flag_addr
 * cleared (0), i.e. it is about to block waiting for the next NMI. In this mode
 * the return-past-entry watermark exit is DISABLED, because such loops reset
 * their own stack (MMX: LDX #$02FF; TXS at $8099), which would otherwise trip
 * the is_ret watermark on the first task RTS. */
static int _interp_run_core(CpuState *cpu, uint32_t entry_pc24,
                                 uint16_t s_exit, uint32_t *out_landing,
                                 uint32_t yield_pc, uint16_t yield_flag_addr,
                                 int reset_cap_on_bounce,
                                 const uint32_t *stop_pcs, int n_stop) {
    /* Local interpreter context → nesting (an AOT bounce that itself traps and
     * re-enters the bridge) gets its own frame; no shared mutable interp. */
    Interp816 in;
    memset(&in, 0, sizeof in);
    in.mem = cpu;
    in.read = bridge_bus_read;
    in.write = bridge_bus_write;
    in.read_word = bridge_bus_read_word;
    in.write_word = bridge_bus_write_word;

    sync_cpu_to_interp(cpu, &in);
    in.k  = (uint8)((entry_pc24 >> 16) & 0xFF);
    in.pc = (uint16)(entry_pc24 & 0xFFFF);

    /* Frame base: the routine has returned to its caller when an RTS/RTL pops
     * cpu->S strictly above this. */
    const uint16_t s_enter = s_exit;

    /* Focused bridge trace: SNESRECOMP_IBRWATCH="lo-hi" (hex pc24). When the
     * bridge entry_pc24 is in range, log every call/ret with sp + the AOT-bounce
     * return value, to localize a tail-dispatch over-pop step by step. */
    int _ibrw = 0;
    {
        static int _iw_init = 0; static long _iw_lo = -1, _iw_hi = -1;
        if (!_iw_init) { _iw_init = 1;
            const char *_e = getenv("SNESRECOMP_IBRWATCH");
            if (_e) sscanf(_e, "%lx-%lx", &_iw_lo, &_iw_hi); }
        if (_iw_lo >= 0 && (long)entry_pc24 >= _iw_lo && (long)entry_pc24 <= _iw_hi) {
            _ibrw = 1;
            fprintf(stderr, "[ibr] ENTER pc=$%06X s_exit=$%04X cpu->S=$%04X\n",
                    (unsigned)entry_pc24, (unsigned)s_exit, (unsigned)cpu->S);
        }
    }

    const int trace = itrace_enabled();
    ITraceEnt head[8], ring[256];
    long itn = 0;

    const long step_cap = interp_step_cap();
    long steps = 0;
    for (; steps < step_cap; steps++) {
        const uint32_t pc_before = ((uint32_t)in.k << 16) | in.pc;
#ifdef SNES_COSIM
        /* Instruction-granular co-sim checkpoint: sync the live interp state into
         * g_cpu (what cosim_state snapshots on the recomp A-side) and offer this
         * opcode boundary. No-op unless SNES_COSIM_SYNC_PC is armed. */
        sync_interp_to_cpu(&in, cpu);
        cosim_insn(pc_before);
#endif
        /* Cooperative-loop yield: stop when the loop reaches its wait point with
         * the vblank flag cleared (one frame's dispatch complete). Checked BEFORE
         * executing so we don't re-enter the spin.
         *
         * Compared bank-mirrored (& 0x7FFFFF), like the stop-PC intercept below:
         * a LoROM scheduler loop re-enters $8099 in whichever of the $00/$80
         * mirror banks the last transfer left in K. MMX's boot walk lands the
         * loop back in bank $00 while yield_pc is given as $80:80A1, so an exact
         * compare never matches — the interp spins the vblank wait to the step
         * cap and bails (JP boot froze here at Task0 state=3). */
        if (yield_pc && (pc_before & 0x7FFFFF) == (yield_pc & 0x7FFFFF) &&
            bridge_bus_read(cpu, yield_flag_addr) == 0) {
            sync_interp_to_cpu(&in, cpu);
            bridge_apu_flush(cpu);
            return 1;
        }
        /* Stop-PC intercept (task-resume mode): JMP/BRA arrival at a PC whose
         * real asm is incompatible with interpretation (fiber-HLE'd machinery
         * like MMX's task-die $80F8). Run its registered HLE body instead and
         * treat the task frame as ended. JSR arrivals never get here — they
         * bounce via the paired-call path below. Compared bank-mirrored. */
        if (n_stop) {
            const uint32_t pc_norm = pc_before & 0x7FFFFF;
            for (int si = 0; si < n_stop; si++) {
                if ((stop_pcs[si] & 0x7FFFFF) == pc_norm) {
                    sync_interp_to_cpu(&in, cpu);
                    bridge_apu_flush(cpu);
                    if (cpu_dispatch_has_entry(cpu, pc_before))
                        cpu_dispatch_pc_paired(cpu, pc_before, 0);
                    return 1;
                }
            }
        }
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

        int _cyc = interp816_runOpcode(&in);   /* executes the opcode; pushes/pops frames */

        /* Guest-time-anchored APU: advance the guest clock + SPC by this opcode's
         * cycles, so the SPC runs continuously during interpreted code (its IPL
         * reaches the $AABB handshake write before the CPU polls it). Mirrors the
         * ref oracle (master = cyc*8 slowROM approx, SPC at the true ratio). Keep
         * g_apu_last_sync_master current so a later AOT bounce's accurate-mode
         * catch-up delta excludes what we already advanced here. */
        if (_cyc <= 0) _cyc = 1;
        {
            uint64_t _master = (uint64_t)_cyc * 8u;
            cpu->cycles        += (uint64_t)_cyc;
            cpu->master_cycles += _master;
#ifdef SNES_COSIM
            /* Shared APU clock (common_rtl.h): the guest-time advance is a
             * per-side clock (master-cycle accounting differs between the
             * interp and compiled models), so under SNES_COSIM_APU_SHARED the
             * SPC is paced ONLY by the HW-touch estimate — identical on both
             * sides of an A/B pair. The opcode's own port access (if any)
             * paces via rtl_accumulate_apu_catchup like compiled code. */
            if (!cosim_apu_shared_clock())
#endif
            {
                /* Guest-time APU, batched (see bridge_apu_flush): accumulate;
                 * convert on APU-port access / ~4096 master / exits. */
                s_apu_pending_master += _master;
                if (s_apu_pending_master >= 4096) bridge_apu_flush(cpu);
            }
        }

        /* Resolved-landing capture (Phase 2 manifest): the PC reached after
         * the FIRST opcode. When entered at an indirect JMP/JML (the
         * unresolved-IndirectGoto tier-down), this is the dynamically resolved
         * target — the actual entry to record, not the JMP site. For a direct
         * dispatch target the caller already knows the entry and ignores it. */
        if (steps == 0 && out_landing)
            *out_landing = ((uint32_t)in.k << 16) | in.pc;

        if (is_call) {
            /* The interp just pushed the real hardware return frame (return-1)
             * and set pc to the target. If the target has a compiled body for
             * the current (m,x), run it compiled. */
            sync_interp_to_cpu(&in, cpu);          /* expose (m,x) + frame to AOT */
            const uint32_t target = ((uint32_t)in.k << 16) | in.pc;
            /* Cooperative-scheduler (yield_pc) mode: NEVER bounce to a compiled
             * body — interpret everything. The scheduler's yield primitive
             * ($808100) is a coroutine switch that saves S and BRAs back into the
             * loop WITHOUT returning to its caller; bouncing it as a normal
             * paired-ABI call (which assumes the callee returns) corrupts the
             * stack / crashes. Pure interpretation follows the BRA and the later
             * TCS-to-saved-S resume faithfully. */
            if (!yield_pc && cpu_dispatch_has_entry(cpu, target)) {
                /* Paired-call ABI: the interp already pushed the return frame, so
                 * run the target with hrv=frame_size and let its RTS/RTL
                 * HOST-RETURN to us (frame popped, S restored to pre-call). We
                 * then resume interpreting at the return address. Using the
                 * dispatch ABI (cpu_dispatch_pc, hrv=0) instead would re-dispatch
                 * on the popped return addr — and over-pop whenever that addr is
                 * itself a registered function entry (e.g. $90:EB55 sub_90EB55
                 * right after HandleChargingBeamGfxAudio's JSR), the Samus-draw
                 * +2 leak. frame: JSL(0x22)=3, JSR/JSR(abs,X)=2. */
                const uint8_t _fs = (op == 0x22) ? 3 : 2;
                uint16_t _sp_pre = in.sp;
                /* Compiled body paces its own APU (RtlApuRead/Write); un-suppress
                 * the per-touch catch-up for its duration, then restore the
                 * PRE-CALL value (not literal 1 — under the co-sim shared APU
                 * clock the flag is 0 for the whole bridge run and must stay 0). */
                bridge_apu_flush(cpu);   /* compiled body must see a current SPC */
                int _apu_drv = g_interp_apu_driving;
                g_interp_apu_driving = 0;
                RecompReturn _air = cpu_dispatch_pc_paired(cpu, target, _fs);
                g_interp_apu_driving = _apu_drv;
                sync_cpu_to_interp(cpu, &in);
                if (_ibrw)
                    fprintf(stderr, "[ibr] call op=$%02X pc=$%06X -> $%06X "
                            "sp_pre=$%04X aot_ret=%d sp_post=$%04X\n",
                            op, (unsigned)pc_before, (unsigned)target,
                            (unsigned)_sp_pre, (int)_air, (unsigned)in.sp);
                if (_air != RECOMP_RETURN_NORMAL) {
                    /* The bounced body did a non-local return that unwound past
                     * this call (it pre-popped to an ancestor and returned an
                     * NLR SKIP). Don't force-resume at ret; treat the interpreted
                     * routine as having exited and let the unwind propagate. */
                    sync_interp_to_cpu(&in, cpu);
                    return 1;
                }
                const uint32_t ret = (pc_before + (uint32_t)call_len) & 0xFFFFFF;
                in.k  = (uint8)((ret >> 16) & 0xFF);
                in.pc = (uint16)(ret & 0xFFFF);
                /* Resume-task mode: a successful bounce is forward progress
                 * (incl. a bounced yield that just slept a frame on its fiber);
                 * the cap should only catch interp-side wedges, not bound the
                 * resumed task's lifetime. */
                if (reset_cap_on_bounce) steps = 0;
            } else if (_ibrw) {
                fprintf(stderr, "[ibr] call op=$%02X pc=$%06X -> $%06X "
                        "(interp into target) sp=$%04X\n",
                        op, (unsigned)pc_before, (unsigned)target, (unsigned)in.sp);
            }
            /* else: no compiled body → keep interpreting into the target
             * (this is the coverage-gap path; Phase 1b records it). */
            continue;
        }

        if (is_ret && !yield_pc) {
            if (_ibrw)
                fprintf(stderr, "[ibr] ret  op=$%02X pc=$%06X sp=$%04X "
                        "(s_enter=$%04X exit=%d)\n",
                        op, (unsigned)pc_before, (unsigned)in.sp,
                        (unsigned)s_enter, (int)((uint16_t)in.sp > s_enter));
            if ((uint16_t)in.sp > s_enter) {
                /* The interpreted routine returned past its entry depth. */
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
        }
    }

    /* Step cap hit — contained bail. Sync so observable state is consistent;
     * the caller treats a 0 return as "gap not cleanly resolved". */
    if (trace) itrace_dump(entry_pc24, head, (int)(itn < 8 ? itn : 8), ring, itn);
    sync_interp_to_cpu(&in, cpu);
    bridge_apu_flush(cpu);
    return 0;
}

/* Wrapper: mark the interp tier as APU-driving for the whole run (nesting-safe
 * save/restore) so rtl_accumulate_apu_catchup skips the per-touch synthetic
 * estimate — the core advances the SPC per opcode instead. */
static int interp_bridge_run_ex2(CpuState *cpu, uint32_t entry_pc24,
                                 uint16_t s_exit, uint32_t *out_landing,
                                 uint32_t yield_pc, uint16_t yield_flag_addr,
                                 int reset_cap_on_bounce,
                                 const uint32_t *stop_pcs, int n_stop) {
    int _saved = g_interp_apu_driving;
#ifdef SNES_COSIM
    /* Shared APU clock: leave the flag clear so interpreted HW touches pace
     * the SPC through the same per-touch path compiled code uses. */
    if (!cosim_apu_shared_clock())
#endif
    g_interp_apu_driving = 1;
    int _r = _interp_run_core(cpu, entry_pc24, s_exit, out_landing, yield_pc,
                              yield_flag_addr, reset_cap_on_bounce, stop_pcs, n_stop);
    g_interp_apu_driving = _saved;
    return _r;
}

/* Public entry: exit watermark = the current stack depth (the routine is
 * entered balanced at cpu->S). */
int interp_bridge_run(CpuState *cpu, uint32_t entry_pc24) {
    return interp_bridge_run_ex2(cpu, entry_pc24, cpu->S, NULL, 0, 0, 0, NULL, 0);
}

/* Save-state task resume: interpret a suspended task from its recorded yield
 * return address (an arbitrary mid-function guest PC) with a caller-supplied
 * base-stack watermark. Calls bounce to compiled bodies via the paired ABI —
 * including the yield HLEs, which suspend the hosting fiber exactly like the
 * compiled path. Returns 1 when the task's top-level RTS unwinds past
 * task_base_s (task finished), 0 on a step-cap wedge bail. The cap resets on
 * every successful bounce, so it bounds interp-side wedges, not task life. */
int interp_bridge_resume_task(CpuState *cpu, uint32_t resume_pc24,
                              uint16_t task_base_s,
                              const uint32_t *stop_pcs, int n_stop) {
    return interp_bridge_run_ex2(cpu, resume_pc24, task_base_s, NULL, 0, 0, 1, stop_pcs, n_stop);
}

/* Faithful LLE of an infinite cooperative-scheduler loop: run the real guest
 * scheduler under interp816 from entry_pc24, dispatching its tasks (which bounce
 * to compiled bodies via the paired ABI), and yield after one frame's slot walk
 * — when the loop reaches yield_pc (its vblank-wait spin) with the flag at
 * flag_addr cleared. Replaces a hand-written C scheduler HLE with the actual
 * ROM code. Returns 1 on clean yield, 0 on step-cap bail. */
int interp_bridge_run_scheduler(CpuState *cpu, uint32_t entry_pc24,
                                uint32_t yield_pc, uint16_t flag_addr) {
    return interp_bridge_run_ex2(cpu, entry_pc24, cpu->S, NULL, yield_pc, flag_addr, 0, NULL, 0);
}

/* ── tier-down entry (called from generated indirect-dispatch defaults) ───── */

extern int snes_frame_counter;

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

/* ── Phase-2 gap manifest: always-on tier-down coverage worklist ───────────
 * One record per distinct (site, target, m/x) tuple. clean_hits = the
 * interpreter ran the gap and returned balanced (a pure coverage gap, safe to
 * promote to AOT); bail_hits = the interpreter hit the step cap and fell back
 * to abandon (the target was unrunnable — a strong signal of an UPSTREAM
 * recomp-state bug at this site, e.g. SM's JMP ($0012)=$FFFF). The offline
 * ingest tool (Phase 3) folds clean discoveries into cfg directives and ranks
 * the bail sites as bug leads. Bounded; an overflow counter never lies about
 * dropped tuples. */
#define TIER2_COVERAGE_MAX 1024
enum { TIER2_KIND_DISPATCH = 0, TIER2_KIND_INDIRECT_GOTO = 1,
       TIER2_KIND_BANK_MISS = 2 };
typedef struct {
    uint32_t site_pc24;
    uint32_t target_pc24;
    uint8_t  mx;    /* ((m_flag&1)<<1)|(x_flag&1): 0=M0X0 1=M0X1 2=M1X0 3=M1X1 */
    uint8_t  kind;  /* TIER2_KIND_* */
    uint64_t clean_hits;
    uint64_t bail_hits;
    int32_t  first_frame;
    int32_t  last_frame;
} Tier2CovSite;
static Tier2CovSite g_tier2_cov[TIER2_COVERAGE_MAX];
static int          g_tier2_cov_count;
static uint64_t     g_tier2_cov_overflow;

static void tier2_record(uint32_t site, uint32_t target, uint8_t mx,
                         uint8_t kind, int clean) {
    int i;
    for (i = 0; i < g_tier2_cov_count; i++) {
        if (g_tier2_cov[i].site_pc24 == site &&
            g_tier2_cov[i].target_pc24 == target &&
            g_tier2_cov[i].mx == mx)
            break;
    }
    if (i == g_tier2_cov_count) {
        if (i >= TIER2_COVERAGE_MAX) { g_tier2_cov_overflow++; return; }
        g_tier2_cov_count++;
        g_tier2_cov[i].site_pc24   = site;
        g_tier2_cov[i].target_pc24 = target;
        g_tier2_cov[i].mx          = mx;
        g_tier2_cov[i].kind        = kind;
        g_tier2_cov[i].clean_hits  = 0;
        g_tier2_cov[i].bail_hits   = 0;
        g_tier2_cov[i].first_frame = snes_frame_counter;
    }
    if (clean) g_tier2_cov[i].clean_hits++;
    else       g_tier2_cov[i].bail_hits++;
    g_tier2_cov[i].last_frame = snes_frame_counter;
}

static uint8_t tier2_entry_mx(const CpuState *cpu) {
    return (uint8_t)(((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1));
}

RecompReturn interp_tier_dispatch(CpuState *cpu, uint32_t target_pc24) {
    interp_tier_note(target_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);
    /* Interpret the routine the static pass couldn't resolve. It shares cpu's
     * stack, so its RTS/RTL pops the inherited caller frame and the bridge
     * exits past entry; control then unwinds to the dispatcher's caller, same
     * as an AOT tail-dispatch would. (Bail -> still NORMAL: contained, the
     * caller continues; a wedged gap is a bug to surface, not to hang on.) */
    int ok = interp_bridge_run(cpu, target_pc24 & 0xFFFFFF);
    /* No site PC at this absolute-indirect default entry; record site==target
     * so the worklist still names the discovered entry. */
    tier2_record(target_pc24 & 0xFFFFFF, target_pc24 & 0xFFFFFF, mx,
                 TIER2_KIND_DISPATCH, ok);
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
    const uint8_t mx = tier2_entry_mx(cpu);
    /* The generated unresolved-IndirectGoto site passes target==site (we
     * re-interpret FROM the JMP itself); a real dispatch default passes the
     * loaded target. */
    const uint8_t kind = (target_pc24 == site_pc24) ? TIER2_KIND_INDIRECT_GOTO
                                                    : TIER2_KIND_DISPATCH;
    uint32_t landing = target_pc24 & 0xFFFFFF;
    /* Unwind watermark is the enclosing function's entry_s (NOT the current S:
     * a PEA+JMP idiom may have pushed a return below entry). Exit when the
     * function RTS/RTLs past entry_s. */
    int ok = interp_bridge_run_ex2(cpu, target_pc24 & 0xFFFFFF, entry_s, &landing, 0, 0, 0, NULL, 0);
    /* For an indirect goto the recorded target is where the JMP actually
     * resolved (the dynamically computed entry); for a dispatch default the
     * passed target already IS the entry. */
    uint32_t rec_target = (kind == TIER2_KIND_INDIRECT_GOTO)
                          ? (landing & 0xFFFFFF) : (target_pc24 & 0xFFFFFF);
    tier2_record(site_pc24 & 0xFFFFFF, rec_target, mx, kind, ok);
    if (ok)
        return RECOMP_RETURN_NORMAL;
    return cpu_unresolved_abandon_balanced(cpu, site_pc24, entry_s, hrv);
}

/* Interpreter-tier fallback for a runtime-pointer JSR (abs,X) call whose
 * loaded target has no AOT body for the live (m,x). cpu_dispatch_call_pc has
 * ALREADY pushed the 2-byte JSR return frame, so:
 *   - watermark = current S (post-push): the target's own RTS pops that
 *     frame and lifts S strictly above the watermark, exiting the bridge.
 *   - post_call = S + 2: the balanced S after the frame is consumed.
 * On a clean return the target's RTS already left S == post_call; on a bail
 * (step cap) we restore post_call ourselves so the frame is discarded and
 * the caller still falls through balanced. Either way return NORMAL — this
 * is a CALL, not a tail dispatch, so it never abandons the caller. Recorded
 * in the tier-2 gap manifest (kind=dispatch) for the worklist. */
RecompReturn interp_tier_run_call(CpuState *cpu, uint32_t target_pc24,
                                  uint32_t source_pc24) {
    target_pc24 &= 0xFFFFFF;
    source_pc24 &= 0xFFFFFF;
    interp_tier_note(target_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);
    const uint16_t watermark = cpu->S;
    const uint16_t post_call = (uint16_t)(cpu->S + 2);
    uint32_t landing = target_pc24;
    int ok = interp_bridge_run_ex2(cpu, target_pc24, watermark, &landing, 0, 0, 0, NULL, 0);
    tier2_record(source_pc24, target_pc24, mx, TIER2_KIND_DISPATCH, ok);
    if (!ok)
        cpu->S = post_call;  /* bail: discard the unconsumed JSR frame */
    return RECOMP_RETURN_NORMAL;
}

/* Phase-4 bank-miss tier-down (opt-in). The generated stub for an untranslated
 * cross-ROM-bank function calls this instead of the no-op trap; we run the
 * real bytes at addr_pc24 (site == target == the function entry). On a bail
 * fall back to the same stack-safe abandon the no-op stub used, so it is never
 * worse than the drop path. Recorded distinctly as kind=bank_miss. */
RecompReturn interp_tier_dispatch_bank_miss(CpuState *cpu, uint32_t addr_pc24,
                                            uint16_t entry_s, uint8_t hrv) {
    addr_pc24 &= 0xFFFFFF;
    interp_tier_note(addr_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);
    uint32_t landing = addr_pc24;
    int ok = interp_bridge_run_ex2(cpu, addr_pc24, entry_s, &landing, 0, 0, 0, NULL, 0);
    tier2_record(addr_pc24, addr_pc24, mx, TIER2_KIND_BANK_MISS, ok);
    if (ok)
        return RECOMP_RETURN_NORMAL;
    return cpu_unresolved_abandon_balanced(cpu, addr_pc24, entry_s, hrv);
}

/* ── manifest serializers ──────────────────────────────────────────────────*/

static const char *tier2_mx_str(uint8_t mx) {
    switch (mx & 3) {
        case 0:  return "M0X0";
        case 1:  return "M0X1";
        case 2:  return "M1X0";
        default: return "M1X1";
    }
}
static const char *tier2_kind_str(uint8_t k) {
    switch (k) {
        case TIER2_KIND_INDIRECT_GOTO: return "indirect_goto";
        case TIER2_KIND_BANK_MISS:     return "bank_miss";
        default:                       return "indirect_dispatch";
    }
}

/* Shared discovery-array body, used by both serializers. */
static void tier2_emit_discoveries(FILE *f, const char *indent) {
    for (int i = 0; i < g_tier2_cov_count; i++) {
        const Tier2CovSite *s = &g_tier2_cov[i];
        fprintf(f,
            "%s%s{\"site_pc24\": \"0x%06X\", \"target_pc24\": \"0x%06X\", "
            "\"entry_mx\": \"%s\", \"site_kind\": \"%s\", "
            "\"clean_hits\": %llu, \"bail_hits\": %llu, "
            "\"first_frame\": %d, \"last_frame\": %d}",
            i ? ",\n" : "\n", indent,
            (unsigned)s->site_pc24, (unsigned)s->target_pc24,
            tier2_mx_str(s->mx), tier2_kind_str(s->kind),
            (unsigned long long)s->clean_hits,
            (unsigned long long)s->bail_hits,
            s->first_frame, s->last_frame);
    }
}

void Tier2CoverageDumpJson(FILE *f) {
    fprintf(f, "  \"tier2_coverage\": {\n"
               "    \"total_tier_hits\": %ld,\n"
               "    \"distinct_sites\": %d,\n"
               "    \"overflowed_tuples\": %llu,\n"
               "    \"discoveries\": [",
            interp_tier_hit_count(), g_tier2_cov_count,
            (unsigned long long)g_tier2_cov_overflow);
    tier2_emit_discoveries(f, "      ");
    fprintf(f, "\n    ]\n  },\n");
}

void Tier2CoverageWriteManifest(const char *path, const char *rom_title) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    /* Minimal title sanitize: drop quotes/backslashes/control so the JSON is
     * always well-formed without a full escaper. Game titles are ASCII. */
    char title[64];
    size_t o = 0;
    if (rom_title) {
        for (const char *p = rom_title; *p && o + 1 < sizeof title; p++) {
            unsigned char c = (unsigned char)*p;
            title[o++] = (c == '"' || c == '\\' || c < 0x20) ? '_' : (char)c;
        }
    }
    title[o] = 0;
    fprintf(f,
        "{\n"
        "  \"schema\": \"snesrecomp tier2 coverage v1\",\n"
        "  \"rom_title\": \"%s\",\n"
        "  \"total_tier_hits\": %ld,\n"
        "  \"distinct_sites\": %d,\n"
        "  \"overflowed_tuples\": %llu,\n"
        "  \"discoveries\": [",
        title, interp_tier_hit_count(), g_tier2_cov_count,
        (unsigned long long)g_tier2_cov_overflow);
    tier2_emit_discoveries(f, "    ");
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}
