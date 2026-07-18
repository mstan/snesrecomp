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
#include "superfx.h"
#include "cosim.h"  /* cosim_insn — instruction-granular lockstep (no-op unless SNES_COSIM) */
#include "common_cpu_infra.h"  /* cpu_take_tailcall_return_context — swallow a stale
                                * tail-armed context on the LLE yield unwind */

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
/* Master-cycle threshold below which the pre-AOT-bounce flush is skipped (see
 * the bounce site). 4096 (~1 output sample of SPC time) matches the periodic
 * batch-flush threshold. Env override SNESRECOMP_LLE_APU_FLUSH_THRESH is a
 * diagnostic lever: 0 restores the old flush-EVERY-bounce behavior. Cached
 * once. */
static uint64_t bridge_bounce_flush_thresh(void) {
    static int64_t s_t = -1;
    if (s_t < 0) {
        const char *e = getenv("SNESRECOMP_LLE_APU_FLUSH_THRESH");
        s_t = (e && e[0]) ? (int64_t)strtoll(e, NULL, 0) : 4096;
        if (s_t < 0) s_t = 0;
    }
    return (uint64_t)s_t;
}
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
uint64_t g_interp_bridge_write_epoch;
static uint64_t s_interp_continuous_read_epoch;
static uint64_t s_interp_dynamic_progress_epoch;
static uint64_t s_interp_bus_master;
static unsigned s_interp_bus_cycles;
static int s_interp_bus_timing_active;
typedef struct BridgeDynamicValue { uint32_t address; uint8_t value, valid; } BridgeDynamicValue;
static BridgeDynamicValue s_bridge_dynamic_values[64];

/* Match Recompiler/snes_cycles.py::region_speed. During an interpreted
 * instruction the callbacks see every real bus transfer (opcode/operand
 * fetches, data, stack and vectors), so charging those addresses directly and
 * the remaining internal cycles at 6 master clocks is both more faithful and
 * less tier-divergent than the old blanket `cpu_cycles * 8` estimate. */
static unsigned bridge_region_speed(uint32_t adr) {
    uint8_t bank=(uint8_t)(adr>>16); uint16_t a=(uint16_t)adr;
    if (bank>=0x40 && bank<=0x7f) return 8;
    if (bank>=0xc0) return g_memsel ? 6 : 8;
    if (a>=0x8000) {
        if (bank<=0x3f) return 8;
        return g_memsel ? 6 : 8;
    }
    if (a<0x2000) return 8;
    if (a<0x4000) return 6;
    if (a<0x4200) return 12;
    if (a<0x6000) return 6;
    return 8;
}

static void bridge_timing_bus(uint32_t adr) {
    if (!s_interp_bus_timing_active) return;
    s_interp_bus_cycles++;
    s_interp_bus_master+=bridge_region_speed(adr);
}

static int bridge_continuous_read(uint32_t adr) {
    uint8_t bank=(uint8_t)(adr>>16); uint16_t a=(uint16_t)adr;
    if (!(bank<=0x3f || (bank>=0x80 && bank<=0xbf))) return 0;
    /* Devices in these windows advance from CPU/master time or from the read
     * protocol itself. Repeating CPU registers around such a read is not a
     * quiescent interrupt wait: keep executing so the device can answer. */
    if (a>=0x2134 && a<0x2180) return 1;       /* PPU counters + APU ports */
    if (a>=0x3000 && a<0x3300) return 1;       /* GSU registers/cache */
    if (a==0x4016 || a==0x4017 || a==0x4212) return 1;
    return 0;
}

static uint8_t bridge_bus_read(void *mem, uint32_t adr) {
    CpuState *cpu = (CpuState *)mem;
    bridge_timing_bus(adr);
    int continuous=bridge_continuous_read(adr);
    if (continuous) s_interp_continuous_read_epoch++;
    if (bridge_is_apu_port(adr)) bridge_apu_flush(cpu);
    uint8_t value=cpu_read8(cpu,(uint8)((adr>>16)&0xff),(uint16)adr);
    if (continuous) {
        BridgeDynamicValue *d=&s_bridge_dynamic_values[(adr^(adr>>8)^(adr>>16))&63];
        if (!d->valid || d->address!=adr || d->value!=value) {
            d->valid=1; d->address=adr; d->value=value;
            s_interp_dynamic_progress_epoch++;
        }
    }
    return value;
}
static void bridge_bus_write(void *mem, uint32_t adr, uint8_t val) {
    bridge_timing_bus(adr);
    g_interp_bridge_write_epoch++;
    CpuState *cpu = (CpuState *)mem;
    if (getenv("SNESRECOMP_APU_PORT_DIAG") && bridge_is_apu_port(adr)) {
        static unsigned reports;
        if(reports++<256) fprintf(stderr,"[apu_port] write $%04X=%02X master=%llu\n",
          (unsigned)(uint16_t)adr,val,(unsigned long long)cpu->master_cycles);
    }
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
    bridge_timing_bus(adrl);
    bridge_timing_bus(adrh);
    if (bridge_continuous_read(adrl) || bridge_continuous_read(adrh))
        s_interp_continuous_read_epoch++;
    CpuState *cpu = (CpuState *)mem;
    if (bridge_is_apu_port(adrl)) bridge_apu_flush(cpu);
    *out = cpu_read16(cpu, (uint8)((adrl >> 16) & 0xFF), (uint16)(adrl & 0xFFFF));
    if (getenv("SNESRECOMP_APU_PORT_DIAG") && (uint16_t)adrl == 0x2140) {
        static uint16_t last=0xffff; static unsigned reports;
        if (*out!=last && reports++<256) {
            fprintf(stderr,"[apu_port] read $2140=%04X pc-master=%llu pending=%llu\n",
                    *out,(unsigned long long)cpu->master_cycles,
                    (unsigned long long)s_apu_pending_master);
            last=*out;
        }
    }
    /* Byte callbacks are bypassed for a claimed word. Treat any changed byte
     * as device progress so long productive transfers do not hit the wedge cap. */
    if (bridge_continuous_read(adrl) || bridge_continuous_read(adrh)) {
        uint8_t lo=(uint8_t)*out, hi=(uint8_t)(*out>>8);
        BridgeDynamicValue *dl=&s_bridge_dynamic_values[(adrl^(adrl>>8)^(adrl>>16))&63];
        BridgeDynamicValue *dh=&s_bridge_dynamic_values[(adrh^(adrh>>8)^(adrh>>16))&63];
        if(!dl->valid||dl->address!=adrl||dl->value!=lo){dl->valid=1;dl->address=adrl;dl->value=lo;s_interp_dynamic_progress_epoch++;}
        if(!dh->valid||dh->address!=adrh||dh->value!=hi){dh->valid=1;dh->address=adrh;dh->value=hi;s_interp_dynamic_progress_epoch++;}
    }
    return true;
}
static bool bridge_bus_write_word(void *mem, uint32_t adrl, uint32_t adrh,
                                  uint16_t val, bool reversed) {
    /* reversed = RMW write-back (high byte first on hardware); keep those on
     * the faithful byte path — WriteRegWord would flip non-APU order. */
    if (reversed || !bridge_hw_word(adrl, adrh)) return false;
    bridge_timing_bus(adrl);
    bridge_timing_bus(adrh);
    g_interp_bridge_write_epoch++;
    CpuState *cpu = (CpuState *)mem;
    if (getenv("SNESRECOMP_APU_PORT_DIAG") && bridge_is_apu_port(adrl)) {
        static unsigned reports;
        if(reports++<256) fprintf(stderr,"[apu_port] writew $%04X=%04X master=%llu\n",
          (unsigned)(uint16_t)adrl,val,(unsigned long long)cpu->master_cycles);
    }
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

static InterpPreOpcodeHook s_pre_opcode_hook;
static uint32_t s_pre_opcode_hook_pc24;

void interp_bridge_set_pre_opcode_hook(uint32_t pc24,
                                       InterpPreOpcodeHook hook) {
    s_pre_opcode_hook_pc24 = pc24 & 0x7FFFFFu;
    s_pre_opcode_hook = hook;
}

/* BRK bridge seam. The bounce is via explicit JSR/JSL interception below, not
 * via planted BRKs, so an interpreted BRK is treated as a no-op continue.
 * (Production hardening may route an unexpected BRK to a contained stop.) */
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

/* ── Fiber-free LLE yield unwind (docs/LLE_SCHEDULER.md) ───────────────────
 * The guest yield primitives of a cooperative scheduler are coroutine
 * switches: they consume the caller's return frame and BRA back into the
 * scheduler loop without ever returning. A compiled task body bounced from
 * the scheduler frame that reaches one cannot host-return through the
 * paired-call chain. The game's LLE-aware yield stub arms this pending
 * unwind instead and returns the LLE sentinel; every emitted callsite
 * propagates it (`return _r - 1`) until the scheduler frame's bounce site
 * consumes it and resumes INTERPRETING at the primitive's real ROM entry —
 * the interpreter then executes the actual coroutine switch byte-exact.
 * Nested non-scheduler bridge frames (tier-2 gap runs) end on the unwind and
 * their tier helpers re-emit the sentinel, so the unwind crosses interleaved
 * compiled/interpreted frames of any depth.
 *
 * s_lle_sched_depth counts scheduler-mode (yield_pc != 0) frames on the host
 * stack; >0 is the "LLE context" the stubs test to pick unwind over fibers. */
static int      s_lle_sched_depth   = 0;
static int      s_lle_unwind_active = 0;
static uint32_t s_lle_unwind_pc24   = 0;
static int      s_lle_unwind_owner_depth = 0;
static uint32_t s_lle_resume_pc24   = 0;
static uint64_t s_lle_master_deadline = 0;
/* Depth of nested interpreter runs and the run that owns the current paired
 * AOT bounce. A rewritten/non-local return from that AOT root must resume the
 * owning interpreter's guest call chain. */
static int      s_interp_bridge_depth = 0;
static int      s_interp_bounce_owner_depth = 0;
/* Recomp-stack depth immediately before any interpreter frame bounces into a
 * paired AOT root. A rewritten return in that root belongs directly to the
 * interpreter; one reached below that root belongs to a compiled ancestor
 * and must first finish/skip that ancestor in the ordinary nested tier path.
 * Saved/restored around each bounce because bridge runs nest. */
static int      s_interp_bounce_recomp_base = -1;
/* Env-gated write-log observability: current interpreter opcode PC, published
 * immediately before execution when SNESRECOMP_WLOG_STATE is armed. */
uint32_t g_interp_wlog_pc24 = 0;
static uint32_t s_lle_bounce_exclusions[16];
static size_t   s_lle_bounce_exclusion_count;

/* Debug lever (SNESRECOMP_LLE_INTERP_TARGET_FILE): a file of hex PC24s, one per
 * line, each forced to the byte interpreter instead of its AOT body. Unbounded
 * (unlike the 16-slot programmatic list) so a whole seeded root set can be
 * disabled at once for structural-vs-execution bisection, editable between runs
 * with no rebuild. Sorted for bsearch; masked to 24 bits. */
static uint32_t *s_interp_file_targets;
static size_t    s_interp_file_count;
static int interp_file_target_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
/* Lazy-load the SNESRECOMP_LLE_INTERP_TARGET_FILE set (idempotent). Shared by
 * the interp->AOT bounce gate and the AOT body-entry guard (rtl_aot_node_denied),
 * so the set is populated no matter which fires first. */
static void ensure_interp_file_targets_loaded(void) {
    static int s_loaded;
    if (s_loaded) return;
    s_loaded = 1;
    const char *fp = getenv("SNESRECOMP_LLE_INTERP_TARGET_FILE");
    if (!fp || !*fp) return;
    FILE *f = fopen(fp, "r");
    if (!f) return;
    size_t cap = 256, n = 0;
    uint32_t *arr = (uint32_t *)malloc(cap * sizeof *arr);
    char line[64];
    while (arr && fgets(line, sizeof line, f)) {
        char *end = NULL;
        unsigned long pc = strtoul(line, &end, 16);
        if (end == line) continue;
        if (n == cap) {
            cap *= 2;
            uint32_t *g = (uint32_t *)realloc(arr, cap * sizeof *arr);
            if (!g) break;
            arr = g;
        }
        arr[n++] = (uint32_t)(pc & 0xFFFFFFu);
    }
    fclose(f);
    if (arr && n) {
        qsort(arr, n, sizeof *arr, interp_file_target_cmp);
        s_interp_file_targets = arr;
        s_interp_file_count = n;
    } else {
        free(arr);
    }
}

/* AOT body-entry guard: generated variant bodies (when emitted with the
 * SNESRECOMP_EMIT_AOT_DENY_GATE codegen flag) call this at the prologue and
 * tier down to the interpreter if their PC is in the deny set. Unlike the
 * interp->AOT bounce gate, this fires for EVERY entry path (direct call, tail,
 * dispatch, alias), so a subset can be soundly disabled for bisection. */
int rtl_aot_node_denied(uint32 pc24) {
    ensure_interp_file_targets_loaded();
    if (!s_interp_file_count) return 0;
    uint32_t key = (uint32_t)pc24 & 0xFFFFFFu;
    return bsearch(&key, s_interp_file_targets, s_interp_file_count,
                   sizeof key, interp_file_target_cmp) != NULL;
}

int interp_bridge_in_lle_scheduler(void) { return s_lle_sched_depth > 0; }
uint32 interp_bridge_lle_resume_pc(void) { return s_lle_resume_pc24; }
void interp_bridge_set_master_deadline(uint64_t master_clock) {
    s_lle_master_deadline = master_clock;
}

RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32 resume_pc24) {
    (void)cpu;
    /* A JMP-reached primitive (task-die / scheduler-dispatch) arrives via a
     * gen tail-call that armed a tailcall return context for a callee that
     * never takes it (the hle wrapper has no prologue). Swallow it here so
     * the NEXT emitted function entered (the next bounce) can't adopt a
     * stale _entry_s/_hrv. */
    cpu_take_tailcall_return_context(NULL, NULL);
    s_lle_unwind_active = 1;
    s_lle_unwind_pc24   = resume_pc24 & 0xFFFFFFu;
    s_lle_unwind_owner_depth = s_interp_bounce_owner_depth;
    return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
}

int interp_bridge_has_direct_paired_bounce(void) {
    if (s_interp_bounce_owner_depth <= 0)
        return 0;
    if (s_interp_bounce_recomp_base < 0 ||
        g_recomp_stack_top <= s_interp_bounce_recomp_base + 1)
        return 1;

    /* A generated root may reach its terminal return through one or more
     * architectural tail transfers. Those transfers add host frames, but
     * every callee inherits the root's _entry_s watermark; semantically they
     * are still the direct paired bounce owned by the active interpreter.
     *
     * A real nested JSR/JSL has a lower entry S and therefore fails this
     * test. Its rewritten return continues to use the compiled-ancestor
     * SKIP path, while a pure tail chain is handed back to the interpreter. */
    const int base = s_interp_bounce_recomp_base;
    const uint16_t root_entry_s = g_cpu_entry_s[base];
    for (int i = base + 1; i < g_recomp_stack_top; i++) {
        if (g_cpu_entry_s[i] != root_entry_s)
            return 0;
    }
    return 1;
}

void interp_bridge_set_lle_bounce_exclusions(const uint32 *targets,
                                              size_t count) {
    if (count > sizeof(s_lle_bounce_exclusions) /
                    sizeof(s_lle_bounce_exclusions[0]))
        count = sizeof(s_lle_bounce_exclusions) /
                sizeof(s_lle_bounce_exclusions[0]);
    s_lle_bounce_exclusion_count = count;
    for (size_t i = 0; i < count; i++)
        s_lle_bounce_exclusions[i] = targets[i] & 0xFFFFFFu;
}

/* Yield-mode bounce switch. Env SNESRECOMP_LLE_BOUNCE overrides; the build
 * default comes from SNESRECOMP_LLE_BOUNCE_DEFAULT so an immature variant
 * can ship interpret-everything while its cfg is enriched (Rockman X JP:
 * its auto-discovered compiled bodies have never passed a fixes pass, and
 * the bounced-vs-interpreted differential splits wholesale at cp2 — bounce
 * stays off there until the tier-2 loop matures the cfg). Rich validated
 * cfgs default ON — compiled task bodies are the point. The env is also the
 * co-sim differential lever: bounced (=1) vs interpreted (=0) must be
 * guest-state bit-exact; any persistent split is a recompiler bug. */
#ifndef SNESRECOMP_LLE_BOUNCE_DEFAULT
#define SNESRECOMP_LLE_BOUNCE_DEFAULT 1
#endif
static int lle_yield_bounce_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SNESRECOMP_LLE_BOUNCE");
        v = (e && e[0]) ? (e[0] != '0') : SNESRECOMP_LLE_BOUNCE_DEFAULT;
    }
    return v;
}

/* Target-scoped LLE differential: keep the rich scheduler/AOT bounce enabled
 * globally while forcing one suspect call through the byte interpreter.
 * Value is a hex PC24 (LoROM mirrors compare equal). */
static int lle_bounce_target_excluded(uint32_t target_pc24) {
    static int s_init;
    static uint32_t s_target = 0xFFFFFFFFu;
    if (!s_init) {
        const char *v = getenv("SNESRECOMP_LLE_INTERP_TARGET");
        if (v && *v)
            s_target = (uint32_t)strtoul(v, NULL, 16) & 0xFFFFFFu;
        ensure_interp_file_targets_loaded();
        s_init = 1;
    }
    if (s_target != 0xFFFFFFFFu &&
        (target_pc24 & 0x7FFFFFu) == (s_target & 0x7FFFFFu))
        return 1;
    for (size_t i = 0; i < s_lle_bounce_exclusion_count; i++) {
        if ((target_pc24 & 0x7FFFFFu) ==
            (s_lle_bounce_exclusions[i] & 0x7FFFFFu))
            return 1;
    }
    if (s_interp_file_count) {
        uint32_t key = target_pc24 & 0xFFFFFFu;
        if (bsearch(&key, s_interp_file_targets, s_interp_file_count,
                    sizeof key, interp_file_target_cmp))
            return 1;
    }
    return 0;
}

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

/* Tier-2 coverage table (definitions below, § gap manifest): shared by the
 * tier-down entries AND the in-bridge gap recorders in the core loop. */
enum { TIER2_KIND_DISPATCH = 0, TIER2_KIND_INDIRECT_GOTO = 1,
       TIER2_KIND_BANK_MISS = 2,
       /* In-bridge sightings (always recorded clean — they are observations,
        * not bounded runs): a JSR/JSL/JSR(abs,X) whose target has no compiled
        * variant for the live (m,x) (the interp runs it inline), and an
        * indirect JMP/JML landing with no compiled variant (JMP arrivals are
        * never bounced). Together these are the cfg-enrichment worklist for
        * minimal-cfg variants (Rockman X JP): tools/tier2_ingest.py folds
        * their targets into `func` directives, the next regen compiles them,
        * and the bridge then bounces instead of interpreting. */
       TIER2_KIND_CALL_GAP = 3, TIER2_KIND_GOTO_GAP = 4 };
static void tier2_record(uint32_t site, uint32_t target, uint8_t mx,
                         uint8_t kind, int clean);
static uint8_t tier2_entry_mx(const CpuState *cpu);

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
                                 uint32_t *out_return_pc,
                                 uint32_t yield_pc, uint16_t yield_flag_addr,
                                 uint8_t yield_flag_value,
                                 int reset_cap_on_bounce,
                                 const uint32_t *stop_pcs, int n_stop,
                                 int stop_on_rti) {
    const int auto_quiescent = yield_pc == 0xFFFFFFFEu;
    /* Local interpreter context → nesting (an AOT bounce that itself traps and
     * re-enters the bridge) gets its own frame; no shared mutable interp. */
    Interp816 in;
    memset(&in, 0, sizeof in);
    in.mem = cpu;
    in.read = bridge_bus_read;
    in.write = bridge_bus_write;
    in.read_word = bridge_bus_read_word;
    in.write_word = bridge_bus_write_word;
    const int wlog_state_sync = getenv("SNESRECOMP_WLOG_STATE") != NULL;

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
    static int dtrace = -1;
    if (dtrace < 0)
        dtrace = getenv("SNESRECOMP_INTERP_DTRACE") ? 1 : 0;
    ITraceEnt head[8], ring[256];
    long itn = 0;

    typedef struct QuiescentState {
        uint32_t pc;
        uint16_t a, x, y, sp, dp;
        uint8_t db, k, c, z, v, n, i, d, mf, xf, e;
        uint64_t write_epoch;
        uint64_t continuous_read_epoch;
        long step;
        unsigned repeats;
    } QuiescentState;
    QuiescentState qring[64];
    memset(qring, 0, sizeof qring);

    const long step_cap = interp_step_cap();
    long steps = 0;
    uint64_t progress_write_epoch=g_interp_bridge_write_epoch;
    uint64_t progress_dynamic_epoch=s_interp_dynamic_progress_epoch;
    for (; steps < step_cap; steps++) {
        const uint32_t pc_before = ((uint32_t)in.k << 16) | in.pc;
        /* IRQs are sampled between instructions.  An auto-quiescent whole-
         * program run must return to its owning scheduler as soon as either
         * the CPU H/V comparator or a coprocessor asserts IRQ; otherwise a
         * perfectly live hardware-poll loop can monopolize the bridge and
         * starve the handler forever. */
        if (auto_quiescent && steps && !in.i && g_snes &&
            (g_snes->inIrq ||
             (g_snes->cart && g_snes->cart->superfx &&
              g_snes->cart->superfx->irq_pending))) {
            s_lle_resume_pc24=pc_before;
            sync_interp_to_cpu(&in,cpu);
            bridge_apu_flush(cpu);
            return 1;
        }
        if (auto_quiescent && s_lle_master_deadline &&
            cpu->master_cycles >= s_lle_master_deadline) {
            s_lle_resume_pc24=pc_before;
            sync_interp_to_cpu(&in,cpu);
            bridge_apu_flush(cpu);
            return 1;
        }
        if (auto_quiescent) {
            QuiescentState now;
            memset(&now, 0, sizeof now);
            now.pc=pc_before; now.a=in.a; now.x=in.x; now.y=in.y;
            now.sp=in.sp; now.dp=in.dp; now.db=in.db; now.k=in.k;
            now.c=in.c; now.z=in.z; now.v=in.v; now.n=in.n; now.i=in.i;
            now.d=in.d; now.mf=in.mf; now.xf=in.xf; now.e=in.e;
            now.write_epoch=g_interp_bridge_write_epoch; now.step=steps;
            now.continuous_read_epoch=s_interp_continuous_read_epoch;
            for (unsigned qi=0; qi<64; qi++) {
                QuiescentState *old=&qring[qi];
                if (old->step && steps-old->step<=64 &&
                    old->pc==now.pc && old->a==now.a && old->x==now.x &&
                    old->y==now.y && old->sp==now.sp && old->dp==now.dp &&
                    old->db==now.db && old->k==now.k && old->c==now.c &&
                    old->z==now.z && old->v==now.v && old->n==now.n &&
                    old->i==now.i && old->d==now.d && old->mf==now.mf &&
                    old->xf==now.xf && old->e==now.e &&
                    old->write_epoch==now.write_epoch &&
                    old->continuous_read_epoch==now.continuous_read_epoch) {
                    now.repeats=old->repeats+1;
                    if (now.repeats>=2) {
                        /* Stable CPU/memory state is a genuine cooperative
                         * wait.  The owning scheduler advances idle hardware
                         * to the next timer comparator or vblank and resumes
                         * here after servicing that event.  Burning the poll
                         * inside the interpreter until any enabled timer fired
                         * made a single host frame consume many guest frames
                         * and could starve rendering.  Live MMIO polls are not
                         * mistaken for this path: continuous_read_epoch changes
                         * on every such read. */
                        s_lle_resume_pc24=pc_before;
                        sync_interp_to_cpu(&in,cpu);
                        bridge_apu_flush(cpu);
                        return 1;
                    }
                    break;
                }
            }
            qring[steps & 63]=now;
        }
        if (s_pre_opcode_hook &&
            (pc_before & 0x7FFFFFu) == s_pre_opcode_hook_pc24) {
            sync_interp_to_cpu(&in, cpu);
            s_pre_opcode_hook(cpu, pc_before);
            sync_cpu_to_interp(cpu, &in);
        }
        /* Opt-in control-flow tripwire: game code normally executes from the
         * LoROM $8000-$FFFF half of a bank.  If a return/jump crosses from ROM
         * into the low I/O/RAM half, capture the transition and a substantial
         * predecessor window immediately; continuing through zero-filled I/O
         * space would otherwise erase the causal tail before a later COP/cap. */
        if (trace && itn > 0 && (pc_before & 0xFFFFu) < 0x8000u) {
            const ITraceEnt *_prev = &ring[(itn - 1) & 255];
            if ((_prev->pc & 0xFFFFu) >= 0x8000u) {
                static int s_low_exec_reports;
                if (s_low_exec_reports < 8) {
                    s_low_exec_reports++;
                    fprintf(stderr,
                            "[interp_low_exec] from=$%06X/%02X to=$%06X "
                            "m=%u x=%u db=$%02X sp=$%04X a=$%04X prior:",
                            (unsigned)_prev->pc, _prev->op,
                            (unsigned)pc_before, in.mf, in.xf, in.db,
                            in.sp, in.a);
                    long _low_start = itn > 64 ? itn - 64 : 0;
                    for (long _li = _low_start; _li < itn; _li++) {
                        const ITraceEnt *_le = &ring[_li & 255];
                        fprintf(stderr, " $%06X/%02X",
                                (unsigned)_le->pc, _le->op);
                    }
                    fputc('\n', stderr);
                }
            }
        }
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
        /* Secondary cooperative hardware polls.  SM uses both
         *   LDA abs; BMI/BPL -5
         *   BIT abs; BMI/BPL -5
         * while NMI/IRQ asynchronously changes bit 15. */
        const uint8_t _poll_op = bridge_bus_read(cpu, pc_before);
        const uint8_t _poll_branch = bridge_bus_read(cpu, pc_before + 3);
        const uint16_t _poll_pc16 = (uint16_t)pc_before;
        const int _secondary_poll_pc =
            _poll_pc16 == 0xE02C || _poll_pc16 == 0xE06B ||
            _poll_pc16 == 0xE50D || _poll_pc16 == 0xE609 ||
            _poll_pc16 == 0xE526;
        if (yield_pc && !auto_quiescent && _secondary_poll_pc &&
            (_poll_op == 0xAD || _poll_op == 0x2C) &&
            (_poll_branch == 0x30 || _poll_branch == 0x10) &&
            bridge_bus_read(cpu, pc_before + 4) == 0xFB) {
            const uint16_t _wait_addr = (uint16_t)(
                bridge_bus_read(cpu, pc_before + 1) |
                (bridge_bus_read(cpu, pc_before + 2) << 8));
            const int _negative =
                (cpu_read16(cpu, in.db, _wait_addr) & 0x8000u) != 0;
            const int _branch_taken =
                _poll_branch == 0x30 ? _negative : !_negative;
            if (_branch_taken) {
                s_lle_resume_pc24 = pc_before;
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
        }
        /* Canonical stable-value poll:
         *
         *     LDA value       ; save the current value in A
         * loop:
         *     CMP value
         *     BEQ loop
         *
         * This is another cooperative interrupt boundary, not an ordinary
         * CPU loop: forward progress requires NMI/IRQ to change memory.  The
         * v2 emitter automatically sends functions containing a pure-memory
         * self-poll through this interpreter path while an LLE scheduler is
         * active.  Match the instruction bytes and the exact self-branch so
         * no game/function address hint is needed.  If the comparison still
         * matches, yield before executing it; the next frame's interrupt can
         * update memory and the resumed CMP then exits naturally.
         *
         * M controls both A and the memory operand width.  Direct-page and
         * indexed variants can be added when observed; absolute CMP is the
         * canonical 65816 form used for interrupt-owned WRAM counters. */
        if (yield_pc && !auto_quiescent && _poll_op == 0xCD &&
            bridge_bus_read(cpu, pc_before + 3) == 0xF0 &&
            bridge_bus_read(cpu, pc_before + 4) == 0xFB) {
            const uint16_t _wait_addr = (uint16_t)(
                bridge_bus_read(cpu, pc_before + 1) |
                (bridge_bus_read(cpu, pc_before + 2) << 8));
            const int _equal = in.mf
                ? ((uint8_t)in.a == cpu_read8(cpu, in.db, _wait_addr))
                : (in.a == cpu_read16(cpu, in.db, _wait_addr));
            if (_equal) {
                s_lle_resume_pc24 = pc_before;
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
        }
        /* Canonical automatic-joypad wait used by synchronous message boxes:
         *
         *   loop: LDA $4212      ; wait for auto-read to finish
         *         BIT #$01
         *         BNE loop
         *         LDA $4218      ; controller 1 low byte/word
         *         BNE done
         *         LDA $4219
         *         BEQ loop
         *
         * With no button held this is intentionally an input-owned blocking
         * loop.  A single-threaded scheduler cannot let it spin atomically:
         * the host must process input and advance a frame before the register
         * can change.  Recognise the register/opcode shape (not a game PC) at
         * the final taken BEQ, yield to the owning LLE scheduler, and resume at
         * the backward target so $4212/$4218/$4219 are sampled again. */
        if (yield_pc && !auto_quiescent && _poll_op == 0xF0 && in.z &&
            bridge_bus_read(cpu, pc_before - 8) == 0xAD &&
            bridge_bus_read(cpu, pc_before - 7) == 0x18 &&
            bridge_bus_read(cpu, pc_before - 6) == 0x42 &&
            bridge_bus_read(cpu, pc_before - 5) == 0xD0 &&
            bridge_bus_read(cpu, pc_before - 4) == 0x05 &&
            bridge_bus_read(cpu, pc_before - 3) == 0xAD &&
            bridge_bus_read(cpu, pc_before - 2) == 0x19 &&
            bridge_bus_read(cpu, pc_before - 1) == 0x42) {
            const int8_t _rel = (int8_t)bridge_bus_read(cpu, pc_before + 1);
            if (_rel < 0) {
                s_lle_resume_pc24 = (pc_before + 2 + _rel) & 0xFFFFFFu;
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
        }
        if (yield_pc && !auto_quiescent &&
            (pc_before & 0x7FFFFF) == (yield_pc & 0x7FFFFF)) {
            const uint8_t _yield_flag = bridge_bus_read(cpu, yield_flag_addr);
            /* Canonical byte wait loop: LDA <flag>; BNE -5.  A synthetic
             * scheduler resume can arrive with DB/M inherited from a compiled
             * coroutine rather than the PHK/PLB + SEP prologue that precedes
             * this loop in ROM.  When the real low flag is clear, repair the
             * instruction-local state before executing LDA; otherwise a
             * 16-bit or foreign-bank read can see an adjacent nonzero byte and
             * spin to the step cap.  Match the bytes and operand so this is a
             * reusable loop contract, not a game/address special case. */
            const int _canonical_wait_loop =
                bridge_bus_read(cpu, pc_before) == 0xAD &&
                bridge_bus_read(cpu, pc_before + 1) ==
                    (uint8_t)yield_flag_addr &&
                bridge_bus_read(cpu, pc_before + 2) ==
                    (uint8_t)(yield_flag_addr >> 8) &&
                bridge_bus_read(cpu, pc_before + 3) == 0xD0 &&
                bridge_bus_read(cpu, pc_before + 4) == 0xFB;
            if (_yield_flag != yield_flag_value && _canonical_wait_loop) {
                in.mf = 1;
                in.db = in.k;
            }
            if (getenv("SNESRECOMP_YIELD_DIAG") &&
                _yield_flag != yield_flag_value && steps > 16) {
                static int _yield_diag_n;
                if (_yield_diag_n < 64) {
                    _yield_diag_n++;
                    fprintf(stderr,
                            "[yield_diag] pc=$%06X flag=$%02X want=$%02X "
                            "m=%u x=%u db=$%02X sp=$%04X a=$%04X z=%u\n",
                            (unsigned)pc_before, _yield_flag,
                            yield_flag_value, in.mf, in.xf, in.db, in.sp,
                            in.a, in.z);
                }
            }
            if (_yield_flag == yield_flag_value) {
                s_lle_resume_pc24 = pc_before;
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
            /* If the validated wait loop recurs after normal execution had a
             * chance to leave it, treat that recurrence as the cooperative
             * block point.  This contains a stale/corrupt flag without a
             * multi-million-instruction spin; the next host frame injects NMI
             * and resumes through the same architectural contract. */
            if (_canonical_wait_loop && steps > 16) {
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
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
        /* A COP reached by interpreted game code vectors to the ROM's invalid-
         * interrupt crash loop.  When the opt-in instruction trace is active,
         * report the first few COP arrivals with their immediate predecessor
         * path before that crash loop erases the useful tail of the ring. */
        if (trace && op == 0x02) {
            static int s_cop_reports;
            if (s_cop_reports < 8) {
                s_cop_reports++;
                fprintf(stderr,
                        "[interp_cop] pc=$%06X m=%u x=%u db=$%02X "
                        "sp=$%04X a=$%04X prior:",
                        (unsigned)pc_before, in.mf, in.xf, in.db,
                        in.sp, in.a);
                long _cop_start = itn > 16 ? itn - 16 : 0;
                for (long _ci = _cop_start; _ci < itn; _ci++) {
                    const ITraceEnt *_ce = &ring[_ci & 255];
                    fprintf(stderr, " $%06X/%02X",
                            (unsigned)_ce->pc, _ce->op);
                }
                fputc('\n', stderr);
            }
        }
        /* Focused mode-switch trace (SNESRECOMP_XCE_TRACE=1): every interpreted
         * XCE with pc/frame/e-before — localizes which guest routine leaves the
         * frame in emulation mode when an A/B run splits on the E flag. */
        {
            static int _xt = -1;
            if (_xt < 0) _xt = getenv("SNESRECOMP_XCE_TRACE") ? 1 : 0;
            if (_xt && op == 0xFB) {
                extern int snes_frame_counter;
                /* post-XCE e = pre-XCE carry */
                fprintf(stderr, "[xce] f=%d pc=$%06X e=%d->%d sp=$%04X\n",
                        snes_frame_counter, (unsigned)pc_before,
                        (int)in.e, (int)in.c, (unsigned)in.sp);
            }
        }
        {
            ITraceEnt _e = { pc_before, op };
            if (itn < 8) head[itn] = _e;
            if (trace) ring[itn & 255] = _e;
            itn++;
        }

        /* Subroutine calls: JSR abs (0x20, 3B), JSL (0x22, 4B),
         * JSR (abs,X) (0xFC, 3B). RTS (0x60) / RTL (0x6B) are returns. */
        const int is_call  = (op == 0x20 || op == 0x22 || op == 0xFC);
        const int call_len = (op == 0x22) ? 4 : 3;
        const int is_ret   = (op == 0x60 || op == 0x6B);

        const uint16_t dp_before = in.dp;
        const uint16_t sp_before = in.sp;
        s_interp_bus_master=0;
        s_interp_bus_cycles=0;
        s_interp_bus_timing_active=1;
        /* Optional first-divergence observability: bus callbacks receive only
         * CpuState, while the live interpreter registers normally stay in the
         * local Interp816 struct until a bridge boundary.  Publish the pre-op
         * state so an address write-log can compare the exact store-site
         * registers against AOT.  Completely inert unless explicitly armed. */
        if (wlog_state_sync) {
            g_interp_wlog_pc24 = pc_before & 0xFFFFFFu;
            sync_interp_to_cpu(&in, cpu);
        }
        int _cyc = interp816_runOpcode(&in);   /* executes the opcode; pushes/pops frames */
        s_interp_bus_timing_active=0;
        if (dtrace && in.dp != dp_before) {
            extern int snes_frame_counter;
            fprintf(stderr,
                    "[interp_d] f=%d pc=$%06X op=$%02X D=$%04X->$%04X "
                    "S=$%04X->$%04X\n",
                    snes_frame_counter, (unsigned)pc_before, (unsigned)op,
                    (unsigned)dp_before, (unsigned)in.dp,
                    (unsigned)sp_before, (unsigned)in.sp);
        }

        /* Guest-time-anchored APU: advance the guest clock + SPC by this opcode's
         * cycles, so the SPC runs continuously during interpreted code (its IPL
         * reaches the $AABB handshake write before the CPU polls it). Mirrors the
         * ref oracle (master = cyc*8 slowROM approx, SPC at the true ratio). Keep
         * g_apu_last_sync_master current so a later AOT bounce's accurate-mode
         * catch-up delta excludes what we already advanced here. */
        if (_cyc <= 0) _cyc = 1;
        {
            unsigned _internal = (unsigned)_cyc > s_interp_bus_cycles
                               ? (unsigned)_cyc - s_interp_bus_cycles : 0;
            uint64_t _master = s_interp_bus_master + (uint64_t)_internal * 6u;
            cpu->cycles        += (uint64_t)_cyc;
            cpu->master_cycles += _master;
            if (g_snes) snes_sync_master_clock(g_snes, cpu->master_cycles);
            if (g_snes && g_snes->cart)
                cart_sync_coprocessors(g_snes->cart, cpu->master_cycles);
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

        if (auto_quiescent &&
            (progress_write_epoch != g_interp_bridge_write_epoch ||
             progress_dynamic_epoch != s_interp_dynamic_progress_epoch)) {
            progress_write_epoch=g_interp_bridge_write_epoch;
            progress_dynamic_epoch=s_interp_dynamic_progress_epoch;
            steps=0;
        }

        /* A host-invoked architectural interrupt handler is paired with the
         * real interrupt frame pushed by cpu_push_interrupt_frame(). Its RTI
         * is the host boundary, just as RTS/RTL crossing the entry watermark
         * is for an ordinary subroutine. The interpreter has already popped
         * P/PC/PB here; host control flow deliberately discards guest PC/PB.
         * Do not continue interpreting at the placeholder return address. */
        if (stop_on_rti && op == 0x40) {
            sync_interp_to_cpu(&in, cpu);
            bridge_apu_flush(cpu);
            return 1;
        }

        /* Resolved-landing capture (Phase 2 manifest): the PC reached after
         * the FIRST opcode. When entered at an indirect JMP/JML (the
         * unresolved-IndirectGoto tier-down), this is the dynamically resolved
         * target — the actual entry to record, not the JMP site. For a direct
         * dispatch target the caller already knows the entry and ignores it. */
        if (steps == 0 && out_landing)
            *out_landing = ((uint32_t)in.k << 16) | in.pc;

        /* In-bridge goto-gap sighting: an indirect JMP/JML's dynamically
         * resolved landing with no compiled variant — task entry points and
         * jump-table targets a minimal cfg hasn't named yet. JMP arrivals are
         * never bounced (no return-frame contract), so without this record
         * they leave no trail at all. JMP (abs)=$6C, JMP (abs,X)=$7C,
         * JML [abs]=$DC. */
        if (op == 0x6C || op == 0x7C || op == 0xDC) {
            const uint32_t landing = ((uint32_t)in.k << 16) | in.pc;
            sync_interp_to_cpu(&in, cpu);   /* live (m,x) for the probe */
            if (!cpu_dispatch_has_entry(cpu, landing))
                tier2_record(pc_before, landing, tier2_entry_mx(cpu),
                             TIER2_KIND_GOTO_GAP, 1);
        }

        if (is_call) {
            /* The interp just pushed the real hardware return frame (return-1)
             * and set pc to the target. If the target has a compiled body for
             * the current (m,x), run it compiled. */
            sync_interp_to_cpu(&in, cpu);          /* expose (m,x) + frame to AOT */
            const uint32_t target = ((uint32_t)in.k << 16) | in.pc;
            /* Cooperative-scheduler (yield_pc) mode bounces too (fiber-free
             * rich-LLE, docs/LLE_SCHEDULER.md): a bounced body that reaches a
             * yield primitive no longer corrupts the paired-call stack — its
             * LLE-aware hle stub arms the yield unwind and the sentinel below
             * brings control back here, where we resume interpreting the real
             * coroutine switch. SNESRECOMP_LLE_BOUNCE=0 restores the
             * interpret-everything behavior (A/B differential lever). */
            const int bounce_ok =
                (!yield_pc || lle_yield_bounce_enabled()) &&
                !lle_bounce_target_excluded(target);
            const int has_body  = cpu_dispatch_has_entry(cpu, target);
            if (bounce_ok && has_body) {
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
                /* Optimization (NOT the turbo-wedge fix — that is the raster-IRQ
                 * decoupling in the game main loop): bring the SPC roughly current
                 * for the compiled body without taking the APU lock on every
                 * bounce. Yield-mode rich-LLE bounces fire thousands of times per
                 * frame (scheduler tick → compiled task body → yield → repeat); an
                 * unconditional flush here is RtlApuLock + a real snes_catchupApu
                 * against the audio thread EVERY bounce — redundant lock traffic in
                 * the same contention class as the fixed per-opcode-lock crawl.
                 * Flush only once the interp has banked ~one output sample of
                 * pending SPC time (>= 4096 master, the same threshold the periodic
                 * batch flush at the accumulate site uses); below that the
                 * staleness the body sees is < 1 sample and its own first port
                 * touch (rtl_accumulate_apu_catchup) closes it. Interp APU-port
                 * accesses still flush unconditionally (bridge_bus_*), so the
                 * correctness-critical upload/handshake reads stay exact. The
                 * pending is a static that persists across the bounce, so no SPC
                 * time is dropped — only deferred to the next real flush. (No-op
                 * under SNES_COSIM_APU_SHARED: pending never accumulates there, so
                 * the co-sim gates are unaffected.) */
                if (s_apu_pending_master >= bridge_bounce_flush_thresh())
                    bridge_apu_flush(cpu);
                int _apu_drv = g_interp_apu_driving;
                g_interp_apu_driving = 0;
                int _saved_bounce_base = s_interp_bounce_recomp_base;
                int _saved_bounce_owner = s_interp_bounce_owner_depth;
                s_interp_bounce_recomp_base = g_recomp_stack_top;
                s_interp_bounce_owner_depth = s_interp_bridge_depth;
                RecompReturn _air = cpu_dispatch_pc_paired(cpu, target, _fs);
                s_interp_bounce_owner_depth = _saved_bounce_owner;
                s_interp_bounce_recomp_base = _saved_bounce_base;
                g_interp_apu_driving = _apu_drv;
                sync_cpu_to_interp(cpu, &in);
                if (_ibrw)
                    fprintf(stderr, "[ibr] call op=$%02X pc=$%06X -> $%06X "
                            "sp_pre=$%04X aot_ret=%d sp_post=$%04X\n",
                            op, (unsigned)pc_before, (unsigned)target,
                            (unsigned)_sp_pre, (int)_air, (unsigned)in.sp);
                if (_air != RECOMP_RETURN_NORMAL) {
                    if (s_lle_unwind_active) {
                        if (s_lle_unwind_owner_depth == s_interp_bridge_depth) {
                            if (getenv("SNESRECOMP_YIELD_STACK_DIAG") &&
                                snes_frame_counter >= 5390) {
                                fprintf(stderr,
                                        "[yield_stack] frame=%d bounce=$%06X "
                                        "sp_pre=$%04X sp_unwind=$%04X "
                                        "resume=$%06X\n",
                                        snes_frame_counter, (unsigned)target,
                                        (unsigned)_sp_pre, (unsigned)in.sp,
                                        (unsigned)s_lle_unwind_pc24);
                            }
                            /* Fiber-free yield: the bounced body reached a
                             * yield primitive; its stub unwound the host
                             * stack to here. Consume the request and resume
                             * interpreting at the primitive's REAL ROM entry
                             * — cpu is exactly as the compiled callsite left
                             * it (JSR frame pushed for JSR-reached
                             * primitives), so the interpreted coroutine
                             * switch runs byte-exact. */
                            s_lle_unwind_active = 0;
                            s_lle_unwind_owner_depth = 0;
                            sync_cpu_to_interp(cpu, &in);
                            in.k  = (uint8)((s_lle_unwind_pc24 >> 16) & 0xFF);
                            in.pc = (uint16)(s_lle_unwind_pc24 & 0xFFFF);
                            if (_ibrw)
                                fprintf(stderr, "[ibr] yield-unwind -> $%06X "
                                        "sp=$%04X\n",
                                        (unsigned)s_lle_unwind_pc24,
                                        (unsigned)in.sp);
                            continue;
                        }
                        /* Nested non-scheduler frame during an active yield
                         * unwind: end this frame; the tier helper that owns
                         * it re-emits the sentinel into its compiled caller
                         * so the unwind keeps propagating. */
                        sync_interp_to_cpu(&in, cpu);
                        return 1;
                    }
                    /* The bounced body did a non-local return that unwound past
                     * this call (it pre-popped to an ancestor and returned an
                     * NLR SKIP). Don't force-resume at ret; treat the interpreted
                     * routine as having exited and let the unwind propagate. */
                    if (yield_pc) {
                        /* Never previously reachable (yield mode didn't
                         * bounce). Ending the scheduler frame restarts the
                         * slot walk next frame at $8099 — contained, but
                         * worth seeing. */
                        static int s_ynlr_logged = 0;
                        if (s_ynlr_logged < 8) {
                            s_ynlr_logged++;
                            fprintf(stderr, "[interp_bridge] yield-mode NLR "
                                    "exit (non-unwind) _air=%d target=$%06X\n",
                                    (int)_air, (unsigned)target);
                        }
                    }
                    sync_interp_to_cpu(&in, cpu);
                    return 1;
                }
                const uint32_t ret =
                    (pc_before + (uint32_t)call_len +
                     (uint32_t)cpu_dispatch_inline_arg_bytes(target)) & 0xFFFFFF;
                in.k  = (uint8)((ret >> 16) & 0xFF);
                in.pc = (uint16)(ret & 0xFFFF);
                /* Resume-task mode: a successful bounce is forward progress
                 * (incl. a bounced yield that just slept a frame on its fiber);
                 * the cap should only catch interp-side wedges, not bound the
                 * resumed task's lifetime. */
                if (reset_cap_on_bounce) steps = 0;
            } else {
                /* No compiled variant for the live (m,x) → keep interpreting
                 * into the target (coverage-gap path). Recorded independent
                 * of bounce POLICY (a bounce-off harvest soak must still see
                 * gaps) but only for genuine gaps — never for targets that
                 * have a body and merely weren't bounced. */
                if (!has_body)
                    tier2_record(pc_before, target, tier2_entry_mx(cpu),
                                 TIER2_KIND_CALL_GAP, 1);
                if (_ibrw)
                    fprintf(stderr, "[ibr] call op=$%02X pc=$%06X -> $%06X "
                            "(interp into target) sp=$%04X\n",
                            op, (unsigned)pc_before, (unsigned)target, (unsigned)in.sp);
            }
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
                if (out_return_pc)
                    *out_return_pc = ((uint32_t)in.k << 16) | in.pc;
                sync_interp_to_cpu(&in, cpu);
                bridge_apu_flush(cpu);
                return 1;
            }
        }
    }

    /* Step cap hit — contained bail. Sync so observable state is consistent;
     * the caller treats a 0 return as "gap not cleanly resolved". */
    {
        static int s_cap_reports;
        if (s_cap_reports < 32) {
            const uint32_t last_pc = ((uint32_t)in.k << 16) | in.pc;
            s_cap_reports++;
            fprintf(stderr,
                    "[interp_cap] entry=$%06X last=$%06X op=$%02X "
                    "m=%u x=%u db=$%02X sp=$%04X a=$%04X flag=$%02X\n",
                    (unsigned)entry_pc24, (unsigned)last_pc,
                    bridge_bus_read(cpu, last_pc), in.mf, in.xf, in.db,
                    in.sp, in.a,
                    yield_pc ? bridge_bus_read(cpu, yield_flag_addr) : 0);
            fprintf(stderr, "[interp_cap] head:");
            for (int hi = 0; hi < 8 && hi < itn; hi++)
                fprintf(stderr, " $%06X/%02X", (unsigned)head[hi].pc,
                        head[hi].op);
            fputc('\n', stderr);
        }
    }
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
                                 uint32_t *out_return_pc,
                                 uint32_t yield_pc, uint16_t yield_flag_addr,
                                 uint8_t yield_flag_value,
                                 int reset_cap_on_bounce,
                                 const uint32_t *stop_pcs, int n_stop,
                                 int stop_on_rti) {
    int _saved = g_interp_apu_driving;
#ifdef SNES_COSIM
    /* Shared APU clock: leave the flag clear so interpreted HW touches pace
     * the SPC through the same per-touch path compiled code uses. */
    if (!cosim_apu_shared_clock())
#endif
    g_interp_apu_driving = 1;
    if (yield_pc) s_lle_sched_depth++;
    s_interp_bridge_depth++;
    int _r = _interp_run_core(cpu, entry_pc24, s_exit, out_landing,
                              out_return_pc, yield_pc,
                              yield_flag_addr, yield_flag_value,
                              reset_cap_on_bounce, stop_pcs, n_stop,
                              stop_on_rti);
    s_interp_bridge_depth--;
    if (yield_pc) {
        s_lle_sched_depth--;
        /* A pending yield unwind must have been consumed by this frame's
         * bounce site; anything still armed here would mis-fire on a later
         * unrelated non-NORMAL return. Contained: clear + log. */
        if (s_lle_unwind_active) {
            s_lle_unwind_active = 0;
            s_lle_unwind_owner_depth = 0;
            fprintf(stderr, "[interp_bridge] stale LLE yield unwind cleared "
                    "at scheduler exit (pc=$%06X)\n",
                    (unsigned)s_lle_unwind_pc24);
        }
    }
    g_interp_apu_driving = _saved;
    return _r;
}

/* Public entry: exit watermark = the current stack depth (the routine is
 * entered balanced at cpu->S). */
int interp_bridge_run(CpuState *cpu, uint32_t entry_pc24) {
    return interp_bridge_run_ex2(cpu, entry_pc24, cpu->S, NULL, NULL,
                                 0, 0, 0, 0, NULL, 0, 0);
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
    return interp_bridge_run_ex2(cpu, resume_pc24, task_base_s, NULL, NULL,
                                 0, 0, 0, 1, stop_pcs, n_stop, 0);
}

/* Faithful LLE of an infinite cooperative-scheduler loop: run the real guest
 * scheduler under interp816 from entry_pc24, dispatching its tasks (which bounce
 * to compiled bodies via the paired ABI), and yield after one frame's slot walk
 * — when the loop reaches yield_pc (its vblank-wait spin) with the flag at
 * flag_addr cleared. Replaces a hand-written C scheduler HLE with the actual
 * ROM code. Returns 1 on clean yield, 0 on step-cap bail. */
int interp_bridge_run_scheduler(CpuState *cpu, uint32_t entry_pc24,
                                uint32_t yield_pc, uint16_t flag_addr) {
    return interp_bridge_run_loop(cpu, entry_pc24, yield_pc, flag_addr, 0);
}

int interp_bridge_run_loop(CpuState *cpu, uint32_t entry_pc24,
                           uint32_t yield_pc, uint16_t flag_addr,
                           uint8_t flag_value) {
    return interp_bridge_run_ex2(cpu, entry_pc24, cpu->S, NULL, NULL, yield_pc,
                                 flag_addr, flag_value, 0, NULL, 0, 0);
}

int interp_bridge_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
    return interp_bridge_run_ex2(cpu, entry_pc24, cpu->S, NULL, NULL,
                                 0xFFFFFFFEu, 0, 0, 0, NULL, 0, 0);
}

int interp_bridge_run_interrupt(CpuState *cpu, uint32_t entry_pc24) {
    return interp_bridge_run_ex2(cpu, entry_pc24, cpu->S, NULL, NULL,
                                 0, 0, 0, 0, NULL, 0, 1);
}

/* ── tier-down entry (called from generated indirect-dispatch defaults) ───── */

extern int snes_frame_counter;

static long s_tier_hits = 0;
long interp_tier_hit_count(void) { return s_tier_hits; }

/* Count each coverage-gap tier-down. The value is exposed through structured
 * coverage manifests and tests; runtime execution does not print per-hit
 * diagnostics. */
static void interp_tier_note(uint32_t target_pc24) {
    (void)target_pc24;
    ++s_tier_hits;
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
#define TIER2_COVERAGE_MAX 4096   /* in-bridge gap sightings (call_gap/goto_gap)
                                   * on a minimal cfg discover far more tuples
                                   * than tier-down entries alone; the overflow
                                   * counter still never lies about drops */
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

static const char *tier2_mx_str(uint8_t mx);
static const char *tier2_kind_str(uint8_t k);

/* Gap tuples remain in this bounded structured table for manifest consumers;
 * runtime execution does not print per-gap diagnostics. */
static void tier2_record(uint32_t site, uint32_t target, uint8_t mx,
                         uint8_t kind, int clean) {
    /* Canonicalize LoROM exec-mirror banks ($80-$BF ≡ $00-$3F) so one guest
     * code path yields ONE tuple regardless of which mirror K held (the LLE
     * scheduler runs in $80; ingest maps target bank -> bankNN.cfg, and
     * there is no bank80.cfg). */
    if (((site   >> 16) & 0xFF) >= 0x80 && ((site   >> 16) & 0xFF) <= 0xBF)
        site   -= 0x800000u;
    if (((target >> 16) & 0xFF) >= 0x80 && ((target >> 16) & 0xFF) <= 0xBF)
        target -= 0x800000u;
    /* Direct-mapped repeat cache: the in-bridge recorders fire once per
     * interpreted call/indirect-jump, so the common case must not re-walk
     * the (up to 4096-entry) table. Index+1 so 0 = empty. */
    static uint16_t s_cache[1024];
    const uint32_t h = (site ^ (target * 2654435761u) ^ mx) & 1023u;
    int i = -1;
    if (s_cache[h]) {
        const int c = (int)s_cache[h] - 1;
        if (c < g_tier2_cov_count &&
            g_tier2_cov[c].site_pc24 == site &&
            g_tier2_cov[c].target_pc24 == target &&
            g_tier2_cov[c].mx == mx)
            i = c;
    }
    if (i < 0) {
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
        s_cache[h] = (uint16_t)(i + 1);
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
    if (s_lle_unwind_active)   /* yield unwound through this nested frame */
        return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
    return RECOMP_RETURN_NORMAL;
}

RecompReturn interp_tier_dispatch_interrupt(CpuState *cpu,
                                            uint32_t target_pc24) {
    cpu_interrupt_context_enter();
    interp_tier_note(target_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);
    int ok = interp_bridge_run_ex2(
        cpu, target_pc24 & 0xFFFFFF, cpu->S, NULL, NULL,
        0, 0, 0, 0, NULL, 0, 1);
    tier2_record(target_pc24 & 0xFFFFFF, target_pc24 & 0xFFFFFF, mx,
                 TIER2_KIND_DISPATCH, ok);
    RecompReturn result = s_lle_unwind_active
        ? (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE
        : RECOMP_RETURN_NORMAL;
    cpu_interrupt_context_leave();
    return result;
}

RecompReturn interp_tier_dispatch_tail(CpuState *cpu, uint32_t target_pc24,
                                       uint32_t site_pc24, uint16_t entry_s,
                                       uint8_t hrv) {
    if (cpu_interrupt_context_active())
        return interp_tier_dispatch_interrupt(cpu, target_pc24);
    /* This tail transfer abandons every compiled guest frame beneath the AOT
     * root that the active interpreter bounced into.  Starting a new nested
     * interpreter here leaves those host frames live; if the guest continuation
     * loops back through another compiled call (DKC2's sprite dispatcher is one
     * example), each iteration recursively nests another AOT/LLE pair until the
     * host stack overflows.  Use the existing arbitrary-depth control-transfer
     * sentinel to unwind to the owning interpreter and resume there at the tail
     * target.  The guest S/register state is untouched, and ordinary top-level
     * AOT tail fallbacks retain the balanced nested-interpreter path below. */
    if (s_interp_bounce_owner_depth > 0)
        return interp_bridge_lle_yield_unwind(cpu, target_pc24);
    return interp_tier_dispatch_balanced(cpu, target_pc24, site_pc24,
                                         entry_s, hrv);
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
    /* Write-log scope: capture the interp's write sequence for a targeted node
     * so it can be first-divergence-diffed against the AOT body. Gated to the
     * function under investigation; a leaf runs exactly its own writes here. */
    static int wlog_target_init = 0;
    static uint32_t wlog_target_pc24 = 0xBB8CB5u;
    if (!wlog_target_init) {
        const char *wlog_target_env = getenv("SNESRECOMP_WLOG_INTERP_TARGET");
        if (wlog_target_env && *wlog_target_env)
            wlog_target_pc24 = (uint32_t)strtoul(
                wlog_target_env, NULL, 16) & 0xFFFFFFu;
        wlog_target_init = 1;
    }
    const int wlog_this = (
        (target_pc24 & 0xFFFFFFu) == wlog_target_pc24);
    if (wlog_this) wlog_scope_enter("interp:scoped_target");
    /* Unwind watermark is the enclosing function's entry_s (NOT the current S:
     * a PEA+JMP idiom may have pushed a return below entry). Exit when the
     * function RTS/RTLs past entry_s. */
    int ok = interp_bridge_run_ex2(cpu, target_pc24 & 0xFFFFFF, entry_s,
                                   &landing, NULL, 0, 0, 0, 0, NULL, 0, 0);
    if (wlog_this) wlog_scope_exit();
    /* For an indirect goto the recorded target is where the JMP actually
     * resolved (the dynamically computed entry); for a dispatch default the
     * passed target already IS the entry. */
    uint32_t rec_target = (kind == TIER2_KIND_INDIRECT_GOTO)
                          ? (landing & 0xFFFFFF) : (target_pc24 & 0xFFFFFF);
    tier2_record(site_pc24 & 0xFFFFFF, rec_target, mx, kind, ok);
    if (s_lle_unwind_active)   /* yield unwound through this nested frame */
        return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
    if (ok) {
        /* A shared suffix may perform a guest non-local return (for example,
         * PLA; PLA; RTS) after an AOT function tails into LLE past its static
         * ownership boundary.  The interpreter has then consumed not only
         * this function's return frame, but one or more compiled ancestors'
         * frames as well.  Resuming the immediate C caller would execute code
         * that the guest already returned past.
         *
         * The generated RTS path resolves the same condition from its
         * pre-RTS S.  A completed interpreter run exposes post-RTS S instead,
         * so use the post-return resolver to translate that architectural
         * stack position into the existing SKIP_N host-unwind contract.  A
         * normal tail return cannot match an ancestor's post-return S and
         * remains NORMAL. */
        const uint16_t expected_post_s = (uint16_t)(entry_s + hrv);
        if (cpu->S != expected_post_s) {
            int skip = cpu_resolve_post_return_skip(cpu->S);
            if (skip > 0)
                return (RecompReturn)skip;
        }
        return RECOMP_RETURN_NORMAL;
    }
    return cpu_unresolved_abandon_balanced(cpu, site_pc24, entry_s, hrv);
}

RecompReturn interp_tier_dispatch_popped_return(CpuState *cpu,
                                                uint32_t target_pc24,
                                                uint32_t site_pc24,
                                                uint16_t miss_restore_s) {
    target_pc24 &= 0xFFFFFFu;
    site_pc24 &= 0xFFFFFFu;
    interp_tier_note(target_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);

    /* cpu_dispatch_pc_from is entered after the previous compiled routine has
     * already popped the RTS/RTL that selected target_pc24.  The target thus
     * inherits the next guest return frame.  Starting from the current S and
     * stopping when its RTS/RTL moves above it exactly matches an hrv=0 AOT
     * tail dispatch; using the enclosing routine's entry_s would conflate two
     * distinct stack frames. */
    const uint16_t target_entry_s = cpu->S;
    uint32_t landing = target_pc24;
    int ok = interp_bridge_run_ex2(cpu, target_pc24, target_entry_s, &landing,
                                   NULL, 0, 0, 0, 0, NULL, 0, 0);
    tier2_record(site_pc24, target_pc24, mx, TIER2_KIND_DISPATCH, ok);
    if (s_lle_unwind_active)
        return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
    if (!ok)
        cpu->S = miss_restore_s;
    return RECOMP_RETURN_NORMAL;
}

RecompReturn interp_tier_dispatch_rewritten_return(CpuState *cpu,
                                                    uint32_t target_pc24,
                                                    uint32_t site_pc24) {
    target_pc24 &= 0xFFFFFFu;
    site_pc24 &= 0xFFFFFFu;

    /* A paired AOT body bounced from an LLE interpreter frame has no compiled
     * guest caller to skip.  If that body deliberately rewrites and pops its
     * return frame (inline arguments are the common case), hand the rewritten
     * continuation back to the already-active interpreter.  The existing LLE
     * unwind sentinel crosses any compiled frames nested inside the bounce;
     * the owning scheduler bridge consumes it and resumes byte interpretation
     * at target_pc24 with the post-return S/register state intact.
     *
     * Interpreting the continuation in a new nested tier frame would run until
     * the interpreted caller itself returned, then manufacture SKIP_N for a
     * host caller that does not exist.  In Super Metroid that abandoned the
     * live scheduler after SpawnHardcodedPlm skipped its four inline bytes and
     * immediately corrupted S.  This branch is context/ABI based, not tied to
     * that function or address. */
    if (interp_bridge_has_direct_paired_bounce())
        return interp_bridge_lle_yield_unwind(cpu, target_pc24);

    interp_tier_note(target_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);
    const uint16_t post_pop_s = cpu->S;
    uint32_t landing = target_pc24;
    int ok = interp_bridge_run_ex2(cpu, target_pc24, post_pop_s, &landing,
                                   NULL, 0, 0, 0, 0, NULL, 0, 0);
    tier2_record(site_pc24, target_pc24, mx, TIER2_KIND_DISPATCH, ok);
    if (s_lle_unwind_active)
        return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
    if (!ok) {
        cpu->S = post_pop_s;
        return RECOMP_RETURN_NORMAL;
    }

    /* Starting at an internal return PC means the interpreter ran inside the
     * paired host caller.  It exits only after an RTS/RTL moves S above the
     * entry watermark, so at least that immediate caller has already returned
     * in guest control flow.  Propagate SKIP_N instead of resuming its C body
     * and executing the epilogue twice.  The stack model normally identifies
     * the exact (possibly deeper) ancestor; SKIP_1 is the conservative minimum
     * for this dedicated rewritten-return path. */
    int skip = cpu_resolve_post_return_skip(cpu->S);
    if (skip < 1) skip = 1;
    return (RecompReturn)skip;
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
RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32_t target_pc24,
                                        uint32_t source_pc24,
                                        uint8_t frame_size,
                                        uint32_t *return_pc24) {
    target_pc24 &= 0xFFFFFF;
    source_pc24 &= 0xFFFFFF;
    interp_tier_note(target_pc24);
    const uint8_t mx = tier2_entry_mx(cpu);
    const uint16_t watermark = cpu->S;
    const uint16_t post_call = (uint16_t)(cpu->S + frame_size);
    uint32_t landing = target_pc24;
    int ok = interp_bridge_run_ex2(cpu, target_pc24, watermark, &landing,
                                   return_pc24, 0, 0, 0, 0, NULL, 0, 0);
    tier2_record(source_pc24, target_pc24, mx, TIER2_KIND_DISPATCH, ok);
    if (s_lle_unwind_active)   /* yield unwound through this nested frame */
        return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
    if (!ok)
        cpu->S = post_call;  /* bail: discard the unconsumed JSR frame */
    return RECOMP_RETURN_NORMAL;
}

RecompReturn interp_tier_run_call(CpuState *cpu, uint32_t target_pc24,
                                  uint32_t source_pc24) {
    return interp_tier_run_call_frame(cpu, target_pc24, source_pc24, 2, NULL);
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
    int ok = interp_bridge_run_ex2(cpu, addr_pc24, entry_s, &landing, NULL,
                                   0, 0, 0, 0, NULL, 0, 0);
    tier2_record(addr_pc24, addr_pc24, mx, TIER2_KIND_BANK_MISS, ok);
    if (s_lle_unwind_active)   /* yield unwound through this nested frame */
        return (RecompReturn)RECOMP_RETURN_LLE_UNWIND_BASE;
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
        case TIER2_KIND_CALL_GAP:      return "call_gap";
        case TIER2_KIND_GOTO_GAP:      return "goto_gap";
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
