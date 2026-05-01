/* cpu_trace.c — backwards-watcher implementation. See cpu_trace.h. */

#include "cpu_trace.h"

#if SNESRECOMP_TRACE

#include <stdio.h>
#include <string.h>

CpuTraceEvent g_cpu_trace_ring[CPU_TRACE_RING_LEN];
uint64_t      g_cpu_trace_idx = 0;
CpuDbpbEvent  g_cpu_dbpb_ring[CPU_DBPB_RING_LEN];
uint64_t      g_cpu_dbpb_idx = 0;
uint8_t       g_db_watch_set = 0;
uint32_t      g_db_watch_bits[8] = {0};
ScopedTripwire g_scoped_tripwire = {0};
PxTripwire     g_px_tripwire = {0};

/* Externs used by px tripwire + scoped tripwire bodies. Defined in
 * common_cpu_infra.c / debug_server.c. Hoisted to file scope so the
 * px tripwire functions (which appear above the existing scoped
 * tripwire externs) can reference them. */
extern const char *g_last_recomp_func;
extern const char *g_recomp_stack[];
extern int         g_recomp_stack_top;

void cpu_trace_arm_px_tripwire(void) {
    /* Reset trip + pmut ring + frozen state so re-arming after a benign
     * earlier trip starts fresh. Preserve the breadcrumbs array — those
     * are the user's checkpoint history. */
    g_px_tripwire.armed = 1;
    g_px_tripwire.triggered = 0;
    g_px_tripwire.pmut_write_idx = 0;
    g_px_tripwire.pmut_count = 0;
    memset(&g_px_tripwire.trip_event, 0, sizeof(g_px_tripwire.trip_event));
    g_px_tripwire.trip_trace_idx = 0;
    g_px_tripwire.A = g_px_tripwire.X = g_px_tripwire.Y = 0;
    g_px_tripwire.S = g_px_tripwire.D = 0;
    g_px_tripwire.DB = g_px_tripwire.PB = g_px_tripwire.P = 0;
    g_px_tripwire.m_flag = g_px_tripwire.x_flag = g_px_tripwire.e_flag = 0;
    g_px_tripwire.stack_depth = 0;
    g_px_tripwire.last_func[0] = 0;
}

void cpu_trace_disarm_px_tripwire(void) {
    g_px_tripwire.armed = 0;
}

void cpu_trace_clear_px_tripwire(void) {
    memset(&g_px_tripwire, 0, sizeof(g_px_tripwire));
}

void cpu_trace_px_record(CpuState *cpu, uint32_t pc24, uint8_t source_kind,
                         uint8_t old_p, uint8_t new_p) {
    if (!g_px_tripwire.armed) return;

    /* Always log into the per-tripwire P-mutation ring (independent of
     * the rotating g_cpu_trace_ring). Stops growing once triggered so the
     * snapshot remains stable. */
    if (!g_px_tripwire.triggered) {
        uint32_t slot = g_px_tripwire.pmut_write_idx % PX_TRIPWIRE_PMUT_RING;
        PxPMutEvent *e = &g_px_tripwire.pmut_ring[slot];
        e->pc24 = pc24;
        e->source_kind = source_kind;
        e->old_p = old_p;
        e->new_p = new_p;
        e->old_x_flag = (old_p & 0x10) ? 1 : 0;
        e->new_x_flag = (new_p & 0x10) ? 1 : 0;
        e->S = cpu ? cpu->S : 0;
        g_px_tripwire.pmut_write_idx++;
        if (g_px_tripwire.pmut_count < PX_TRIPWIRE_PMUT_RING) {
            g_px_tripwire.pmut_count++;
        }
    }

    /* Fire on first P.X 1→0 transition. */
    uint8_t old_x = (old_p & 0x10) ? 1 : 0;
    uint8_t new_x = (new_p & 0x10) ? 1 : 0;
    if (g_px_tripwire.triggered) return;
    if (!(old_x == 1 && new_x == 0)) return;

    g_px_tripwire.triggered = 1;
    g_px_tripwire.trip_event.pc24 = pc24;
    g_px_tripwire.trip_event.source_kind = source_kind;
    g_px_tripwire.trip_event.old_p = old_p;
    g_px_tripwire.trip_event.new_p = new_p;
    g_px_tripwire.trip_event.old_x_flag = old_x;
    g_px_tripwire.trip_event.new_x_flag = new_x;
    g_px_tripwire.trip_event.S = cpu ? cpu->S : 0;
    g_px_tripwire.trip_trace_idx = (uint32_t)g_cpu_trace_idx;

    if (cpu) {
        g_px_tripwire.A = cpu->A;
        g_px_tripwire.X = cpu->X;
        g_px_tripwire.Y = cpu->Y;
        g_px_tripwire.S = cpu->S;
        g_px_tripwire.D = cpu->D;
        g_px_tripwire.DB = cpu->DB;
        g_px_tripwire.PB = cpu->PB;
        g_px_tripwire.P = cpu->P;
        g_px_tripwire.m_flag = cpu->m_flag;
        g_px_tripwire.x_flag = cpu->x_flag;
        g_px_tripwire.e_flag = cpu->emulation;
    }

    if (g_last_recomp_func) {
        strncpy(g_px_tripwire.last_func, g_last_recomp_func,
                PX_TRIPWIRE_FUNC_LEN - 1);
        g_px_tripwire.last_func[PX_TRIPWIRE_FUNC_LEN - 1] = 0;
    }

    int depth = g_recomp_stack_top;
    if (depth > PX_TRIPWIRE_STACK_DEPTH) depth = PX_TRIPWIRE_STACK_DEPTH;
    g_px_tripwire.stack_depth = depth;
    int skip = g_recomp_stack_top - depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) {
            strncpy(g_px_tripwire.stack[i], p, PX_TRIPWIRE_FUNC_LEN - 1);
            g_px_tripwire.stack[i][PX_TRIPWIRE_FUNC_LEN - 1] = 0;
        } else {
            g_px_tripwire.stack[i][0] = 0;
        }
    }
}

void cpu_trace_px_breadcrumb(CpuState *cpu, uint32_t marker, const char *label) {
    if (g_px_tripwire.breadcrumb_count >= PX_BREADCRUMB_MAX) return;
    PxBreadcrumb *bc = &g_px_tripwire.breadcrumbs[g_px_tripwire.breadcrumb_count++];
    bc->marker = marker;
    if (cpu) {
        bc->P = cpu->P;
        bc->m_flag = cpu->m_flag;
        bc->x_flag = cpu->x_flag;
        bc->e_flag = cpu->emulation;
        bc->A = cpu->A; bc->X = cpu->X; bc->Y = cpu->Y;
        bc->S = cpu->S; bc->D = cpu->D;
        bc->DB = cpu->DB; bc->PB = cpu->PB;
    }
    if (label) {
        strncpy(bc->label, label, PX_TRIPWIRE_FUNC_LEN - 1);
        bc->label[PX_TRIPWIRE_FUNC_LEN - 1] = 0;
    }
}

/* External symbols from common_cpu_infra / debug_server. */
extern const char *g_last_recomp_func;
extern const char *g_recomp_stack[];
extern int         g_recomp_stack_top;
extern int         snes_frame_counter;
extern uint64_t          g_main_cpu_cycles_estimate;
extern volatile uint64_t g_block_counter;
extern uint8_t     g_ram[];

/* Forward decl — definition below in this TU. The arm function needs it
 * to canonicalize (bank, addr) → ram_off at arm time so the fire path
 * can compare a single integer instead of replaying mirror logic. */
static int32_t wram_offset(uint8_t bank, uint16_t addr);

void cpu_trace_arm_scoped_tripwire(uint8_t bank, uint16_t addr_lo,
                                   uint16_t addr_hi, const char *scope_substr) {
    cpu_trace_arm_scoped_tripwire_v(bank, addr_lo, addr_hi, scope_substr, 0);
    /* Disable value-match (the v-variant defaults match_enabled=1; we
     * call it with 0 here but the v-variant treats the value-arg as
     * "match anything when match_enabled=0"). Set explicitly. */
    g_scoped_tripwire.match_enabled = 0;
}

void cpu_trace_arm_scoped_tripwire_v(uint8_t bank, uint16_t addr_lo,
                                     uint16_t addr_hi, const char *scope_substr,
                                     uint8_t match_val) {
    memset(&g_scoped_tripwire, 0, sizeof(g_scoped_tripwire));
    g_scoped_tripwire.armed = 1;
    g_scoped_tripwire.bank = bank;
    g_scoped_tripwire.addr_lo = addr_lo;
    g_scoped_tripwire.addr_hi = addr_hi;
    /* Pre-compute canonical ram_off range using the same mapping cpu_write*
     * uses, so writes through DB-mirror banks (e.g. $00:0100) fire too. */
    g_scoped_tripwire.ram_off_lo = wram_offset(bank, addr_lo);
    g_scoped_tripwire.ram_off_hi = wram_offset(bank, addr_hi);
    if (g_scoped_tripwire.ram_off_lo < 0 || g_scoped_tripwire.ram_off_hi < 0) {
        /* Bad range — disarm to avoid surprise misses. */
        g_scoped_tripwire.armed = 0;
    }
    if (scope_substr) {
        strncpy(g_scoped_tripwire.scope_substr, scope_substr,
                SCOPED_TRIPWIRE_FUNC_LEN - 1);
        g_scoped_tripwire.scope_substr[SCOPED_TRIPWIRE_FUNC_LEN - 1] = 0;
    }
    g_scoped_tripwire.match_enabled = 1;
    g_scoped_tripwire.match_val = match_val;
}

void cpu_trace_disarm_scoped_tripwire(void) {
    g_scoped_tripwire.armed = 0;
}

/* Walk backward over the trace ring to find the most recent BLOCK and
 * FUNC_ENTRY events before `before_idx`. Records their pc24 into the
 * tripwire snapshot so the client can locate the writer in source. */
static void scoped_tripwire_collect_recent_context(uint64_t before_idx) {
    g_scoped_tripwire.recent_block_pc24 = 0;
    g_scoped_tripwire.recent_func_pc24 = 0;
    int got_block = 0, got_func = 0;
    /* Limit to last 4096 events to bound work. */
    int scan = 4096;
    for (int i = 1; i <= scan && (got_block == 0 || got_func == 0); i++) {
        if ((uint64_t)i > before_idx) break;
        uint64_t abs_idx = before_idx - i;
        int slot = (int)(abs_idx & (CPU_TRACE_RING_LEN - 1));
        CpuTraceEvent *e = &g_cpu_trace_ring[slot];
        if (!got_block && e->event_type == CPU_TR_BLOCK) {
            g_scoped_tripwire.recent_block_pc24 = e->pc24;
            got_block = 1;
        }
        if (!got_func && e->event_type == CPU_TR_FUNC_ENTRY) {
            g_scoped_tripwire.recent_func_pc24 = e->pc24;
            got_func = 1;
        }
    }
}

/* Called from cpu_trace_wram_write_check on every match. Returns 1 if
 * armed-and-fired (caller can skip future checks for this watch). */
static int scoped_tripwire_maybe_fire(CpuState *cpu, uint8_t bank,
                                      uint16_t addr, uint8_t hit_byte_in_word,
                                      uint8_t hit_val, int width) {
    ScopedTripwire *t = &g_scoped_tripwire;
    if (!t->armed || t->triggered) return 0;
    /* Use canonical ram_off, not (bank, addr). Bank-strict matching
     * silently missed writes routed through DB-mirror banks ($00:0100,
     * $80:0100, ...), which is the dominant SMW pattern for low-DP
     * stores. */
    int32_t off = wram_offset(bank, addr);
    if (off < 0) return 0;
    if (off < t->ram_off_lo || off > t->ram_off_hi) return 0;
    /* Optional value-match: only fire when the byte that lands at the
     * watched offset equals match_val. For 16-bit STAs this checks the
     * specific byte (low or high) — same hit_val that the per-watch
     * recorder records. */
    if (t->match_enabled && hit_val != t->match_val) return 0;
    /* Scope check: substring of any recomp stack entry. */
    if (t->scope_substr[0]) {
        int found = 0;
        for (int i = 0; i < g_recomp_stack_top && !found; i++) {
            const char *p = g_recomp_stack[i];
            if (p && strstr(p, t->scope_substr)) found = 1;
        }
        if (!found) return 0;
    }
    /* Trigger: capture full snapshot. */
    t->triggered = 1;
    t->frame = snes_frame_counter;
    t->main_cycles = g_main_cpu_cycles_estimate;
    t->trace_idx = g_cpu_trace_idx;  /* points one PAST the WRAM_WRITE event */
    t->block_counter = g_block_counter;
    t->hit_addr = addr;
    t->hit_val = hit_val;
    t->hit_byte_in_word = hit_byte_in_word;
    t->width_seen = (uint8_t)width;
    t->A = cpu->A; t->X = cpu->X; t->Y = cpu->Y;
    t->S = cpu->S; t->D = cpu->D;
    t->DB = cpu->DB; t->PB = cpu->PB; t->P = cpu->P;
    t->m_flag = cpu->m_flag; t->x_flag = cpu->x_flag;
    t->e_flag = cpu->emulation;

    scoped_tripwire_collect_recent_context(t->trace_idx);

    if (g_last_recomp_func) {
        strncpy(t->last_func_name, g_last_recomp_func,
                SCOPED_TRIPWIRE_CONTEXT_FN - 1);
        t->last_func_name[SCOPED_TRIPWIRE_CONTEXT_FN - 1] = 0;
    }

    /* Recomp stack snapshot (deepest first; matches g_recomp_stack order). */
    int depth = g_recomp_stack_top;
    if (depth > SCOPED_TRIPWIRE_STACK_DEPTH) depth = SCOPED_TRIPWIRE_STACK_DEPTH;
    t->stack_depth = depth;
    int skip = g_recomp_stack_top - depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) {
            strncpy(t->stack[i], p, SCOPED_TRIPWIRE_FUNC_LEN - 1);
            t->stack[i][SCOPED_TRIPWIRE_FUNC_LEN - 1] = 0;
        } else {
            t->stack[i][0] = 0;
        }
    }

    /* DP region snapshot ($7E:0080-009F = WRAM offset 0x0080) */
    memcpy(t->dp_snapshot, &g_ram[0x0080], SCOPED_TRIPWIRE_DP_BYTES);
    /* DP-low snapshot ($7E:0000-001F) — captures source ptrs used by
     * palette/stripe loops (DP $00-$02 = 24-bit ptr in many SMW routines). */
    memcpy(t->dp_low_snapshot, &g_ram[0x0000], 32);
    /* GameMode region snapshot ($7E:0100-010F = WRAM offset 0x0100) */
    memcpy(t->gm_snapshot, &g_ram[0x0100], SCOPED_TRIPWIRE_GM_BYTES);

    return 1;
}

static void capture(CpuState *cpu, uint32_t pc24, uint8_t event_type,
                    uint8_t extra0, uint16_t extra1) {
    int slot = (int)(g_cpu_trace_idx++ & (CPU_TRACE_RING_LEN - 1));
    CpuTraceEvent *e = &g_cpu_trace_ring[slot];
    e->pc24 = pc24;
    e->native_func_id_or_hash = 0;  /* set separately by func_entry */
    e->A = cpu->A;
    e->X = cpu->X;
    e->Y = cpu->Y;
    e->S = cpu->S;
    e->D = cpu->D;
    e->DB = cpu->DB;
    e->PB = cpu->PB;
    e->P = cpu->P;
    e->M = cpu->m_flag;
    e->XF = cpu->x_flag;
    e->event_type = event_type;
    e->extra0 = extra0;
    e->extra1 = extra1;
}

void cpu_trace_block(CpuState *cpu, uint32_t pc24) {
    capture(cpu, pc24, CPU_TR_BLOCK, 0, 0);
    /* Stack-range tripwire — fires once when S first leaves the
     * configured range. Disarms after firing to avoid spam. */
    extern uint8_t  g_s_watch_set;
    extern uint16_t g_s_watch_lo;
    extern uint16_t g_s_watch_hi;
    if (g_s_watch_set && (cpu->S < g_s_watch_lo || cpu->S > g_s_watch_hi)) {
        g_s_watch_set = 0;  /* one-shot */
        char tag[96];
        snprintf(tag, sizeof(tag),
                 "S-WATCH HIT S=$%04X (range $%04X-$%04X) at PC $%06X",
                 cpu->S, g_s_watch_lo, g_s_watch_hi, pc24);
        cpu_trace_dump_dbpb(tag);
        cpu_trace_dump_recent(tag, 128);
    }
}

/* Tiny FNV-1a over a NUL-terminated function name. */
static uint32_t fnv1a(const char *s) {
    uint32_t h = 0x811C9DC5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) {
    int slot = (int)(g_cpu_trace_idx++ & (CPU_TRACE_RING_LEN - 1));
    CpuTraceEvent *e = &g_cpu_trace_ring[slot];
    e->pc24 = pc24;
    uint32_t h = name ? fnv1a(name) : 0;
    e->native_func_id_or_hash = h;
    e->A = cpu->A;
    e->X = cpu->X;
    e->Y = cpu->Y;
    e->S = cpu->S;
    e->D = cpu->D;
    e->DB = cpu->DB;
    e->PB = cpu->PB;
    e->P = cpu->P;
    e->M = cpu->m_flag;
    e->XF = cpu->x_flag;
    e->event_type = CPU_TR_FUNC_ENTRY;
    e->extra0 = 0;
    e->extra1 = 0;
    /* Function-name tripwire: one-shot dump if entered. */
    extern uint32_t g_func_watch_hash;
    extern const char *g_func_watch_name;
    if (g_func_watch_hash && g_func_watch_hash == h) {
        char tag[120];
        snprintf(tag, sizeof(tag), "FUNC-WATCH HIT %s at PC $%06X",
                 g_func_watch_name ? g_func_watch_name : "?", pc24);
        g_func_watch_hash = 0;  /* one-shot */
        cpu_trace_dump_dbpb(tag);
        cpu_trace_dump_recent(tag, 128);
    }
}

void cpu_trace_event(CpuState *cpu, uint32_t pc24, uint8_t event_type,
                     uint8_t extra0, uint16_t extra1) {
    capture(cpu, pc24, event_type, extra0, extra1);
}

static int db_watch_hit(uint8_t db) {
    return (g_db_watch_bits[db >> 5] >> (db & 0x1F)) & 1u;
}

void cpu_trace_db_change(CpuState *cpu, uint32_t pc24, uint8_t old_db,
                         uint8_t new_db, uint8_t event_type) {
    capture(cpu, pc24, event_type, old_db, (uint16_t)new_db);
    int slot = (int)(g_cpu_dbpb_idx++ & (CPU_DBPB_RING_LEN - 1));
    CpuDbpbEvent *d = &g_cpu_dbpb_ring[slot];
    d->pc24 = pc24;
    d->event_type = event_type;
    d->reg_id = 0; /* DB */
    d->old_val = old_db;
    d->new_val = new_db;
    d->S = cpu->S;
    d->pad = 0;
    if (g_db_watch_set && db_watch_hit(new_db) && new_db != old_db) {
        char tag[64];
        snprintf(tag, sizeof(tag), "DB-WATCH HIT $%02X (was $%02X) at PC $%06X",
                 new_db, old_db, pc24);
        cpu_trace_dump_dbpb(tag);
        cpu_trace_dump_recent(tag, 256);
    }
}

void cpu_trace_pb_change(CpuState *cpu, uint32_t pc24, uint8_t old_pb,
                         uint8_t new_pb, uint8_t event_type) {
    capture(cpu, pc24, event_type, old_pb, (uint16_t)new_pb);
    int slot = (int)(g_cpu_dbpb_idx++ & (CPU_DBPB_RING_LEN - 1));
    CpuDbpbEvent *d = &g_cpu_dbpb_ring[slot];
    d->pc24 = pc24;
    d->event_type = event_type;
    d->reg_id = 1; /* PB */
    d->old_val = old_pb;
    d->new_val = new_pb;
    d->S = cpu->S;
    d->pad = 0;
}

void cpu_trace_set_db_watch(uint8_t db_byte, int enabled) {
    if (enabled) {
        g_db_watch_bits[db_byte >> 5] |= (1u << (db_byte & 0x1F));
        g_db_watch_set = 1;
    } else {
        g_db_watch_bits[db_byte >> 5] &= ~(1u << (db_byte & 0x1F));
        int any = 0;
        for (int i = 0; i < 8; i++) if (g_db_watch_bits[i]) { any = 1; break; }
        g_db_watch_set = (uint8_t)any;
    }
}

/* PB tripwire (mirror of DB). cpu_trace_pb_change checks against this. */
uint8_t       g_pb_watch_set = 0;
uint32_t      g_pb_watch_bits[8] = {0};
void cpu_trace_set_pb_watch(uint8_t pb_byte, int enabled) {
    if (enabled) {
        g_pb_watch_bits[pb_byte >> 5] |= (1u << (pb_byte & 0x1F));
        g_pb_watch_set = 1;
    } else {
        g_pb_watch_bits[pb_byte >> 5] &= ~(1u << (pb_byte & 0x1F));
        int any = 0;
        for (int i = 0; i < 8; i++) if (g_pb_watch_bits[i]) { any = 1; break; }
        g_pb_watch_set = (uint8_t)any;
    }
}

/* Stack-pointer-range tripwire — fires when S leaves the configured
 * range. SMW's normal stack lives in $0100-$1FFF. Excursions outside
 * that mean either MVN/MVP gone wild, TXS with bad X, or unbalanced
 * push/pull from the C-call vs SNES-stack mismatch. */
uint8_t  g_s_watch_set = 0;
uint16_t g_s_watch_lo = 0x0100;
uint16_t g_s_watch_hi = 0x1FFF;
void cpu_trace_set_s_range_watch(uint16_t s_lo, uint16_t s_hi, int enabled) {
    g_s_watch_lo = s_lo;
    g_s_watch_hi = s_hi;
    g_s_watch_set = enabled ? 1 : 0;
}

/* Function-name watch (matched by FNV-1a hash inside cpu_trace_func_entry). */
uint32_t g_func_watch_hash = 0;
const char *g_func_watch_name = NULL;
void cpu_trace_set_func_watch(const char *name) {
    extern uint32_t fnv1a_extern(const char *s);
    g_func_watch_name = name;
    g_func_watch_hash = name ? fnv1a_extern(name) : 0;
}

/* WRAM-address watches.
 *
 * The check loop in cpu_trace_wram_write_check is on the hot path of
 * every gen-code byte/word store, so it has to be cheap. The big lever
 * is `g_wram_watch_any`: when no watches are armed it returns instantly.
 * Once a watch is armed, the linear scan over CPU_WRAM_WATCH_MAX slots
 * is fine — typical use has <4 watches and the slot count is tiny.
 *
 * SNES WRAM mirroring: low banks $00-$3F:0000-1FFF and bank $7E:0000-1FFF
 * alias the same physical RAM. We store the canonical g_ram offset in
 * the watch and compare against the offset the caller computed; that
 * way (bank=$7E, addr=$008c) and (bank=$00, addr=$008c) trip the same
 * watch without us having to know about mirroring at check time. */
WramWatch g_wram_watches[CPU_WRAM_WATCH_MAX];
uint8_t   g_wram_watch_any = 0;

/* Compute the g_ram offset for a (bank, addr) pair. Mirrors the logic
 * in cpu_state.c::cpu_ram_offset; duplicated here so we can resolve a
 * watch's offset at arming time without including cpu_state internals. */
static int32_t wram_offset(uint8_t bank, uint16_t addr) {
    if (bank == 0x7E) return (int32_t)addr;
    if (bank == 0x7F) return 0x10000 + (int32_t)addr;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        return (int32_t)addr;
    }
    return -1;
}

void cpu_trace_set_wram_watch(uint8_t bank, uint16_t addr, int width,
                              int match_value, uint8_t value, int enabled) {
    int32_t off = wram_offset(bank, addr);
    if (off < 0) {
        fprintf(stderr, "[cpu_trace] WRAM-watch IGNORED: $%02X:%04X is not WRAM\n",
                bank, addr);
        fflush(stderr);
        return;
    }
    /* If an existing slot matches (offset, match_value, value), update
     * its enabled state — don't double-arm the same watch. */
    int slot = -1;
    for (int i = 0; i < CPU_WRAM_WATCH_MAX; i++) {
        WramWatch *w = &g_wram_watches[i];
        if (w->enabled && w->ram_offset == off &&
            (uint8_t)(match_value ? 1 : 0) == w->match_value &&
            (!match_value || w->value == value)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < CPU_WRAM_WATCH_MAX; i++) {
            if (!g_wram_watches[i].enabled) { slot = i; break; }
        }
    }
    if (slot < 0) {
        fprintf(stderr, "[cpu_trace] WRAM-watch table FULL (max=%d)\n",
                CPU_WRAM_WATCH_MAX);
        fflush(stderr);
        return;
    }
    WramWatch *w = &g_wram_watches[slot];
    w->enabled     = enabled ? 1 : 0;
    w->match_value = (uint8_t)(match_value ? 1 : 0);
    w->value       = value;
    w->width       = (width == 2) ? 2 : 1;
    w->ram_offset  = off;
    w->bank        = bank;
    w->addr        = addr;
    /* Refresh global "any" flag. */
    int any = 0;
    for (int i = 0; i < CPU_WRAM_WATCH_MAX; i++) {
        if (g_wram_watches[i].enabled) { any = 1; break; }
    }
    g_wram_watch_any = (uint8_t)any;
    fprintf(stderr, "[cpu_trace] WRAM-watch %s slot=%d $%02X:%04X off=%05X "
                    "%s%s%02X width=%d\n",
            enabled ? "ARMED" : "DISARMED",
            slot, bank, addr, (uint32_t)off,
            match_value ? "==" : "any",
            match_value ? "$" : "",
            match_value ? value : 0,
            w->width);
    fflush(stderr);
}

void cpu_trace_clear_wram_watches(void) {
    memset(g_wram_watches, 0, sizeof(g_wram_watches));
    g_wram_watch_any = 0;
}

void cpu_trace_wram_write_check(CpuState *cpu, uint8_t bank, uint16_t addr,
                                int32_t ram_off, uint16_t new_val, int width) {
    /* Scoped tripwire fires regardless of g_wram_watch_any — it has its
     * own armed/triggered gate. Check it here BEFORE the early-out so a
     * client can arm a tripwire without needing a parallel cpu_trace
     * watch slot. */
    if (g_scoped_tripwire.armed && !g_scoped_tripwire.triggered && ram_off >= 0) {
        uint8_t b0 = (uint8_t)(new_val & 0xFF);
        uint8_t b1 = (uint8_t)(new_val >> 8);
        scoped_tripwire_maybe_fire(cpu, bank, addr, 0, b0, width);
        if (width == 2 && !g_scoped_tripwire.triggered) {
            scoped_tripwire_maybe_fire(cpu, bank, (uint16_t)(addr + 1), 1, b1, width);
        }
    }
    if (!g_wram_watch_any || ram_off < 0) return;
    /* Always-on recorder. Every write to a watched offset goes into the
     * main trace ring as a CPU_TR_WRAM_WRITE event. The watch does NOT
     * disarm on record — by the time anyone connects to query, the
     * earliest writes (e.g. boot-time stores) are already in the ring,
     * and the user reads backward from "now" to find the writer of
     * interest. (This is the always-on-ring-buffer pattern in
     * CLAUDE.md: never time/attach for observability.)
     *
     * The match_value flag is now a TRIPWIRE-ONLY add-on: when set, the
     * first write whose value matches `value` ALSO auto-dumps the
     * trace+dbpb rings to stderr and disarms the tripwire (NOT the
     * recording — the slot stays armed and keeps recording subsequent
     * writes). Tripwires are useful when you know the bad value
     * up-front and want a stderr dump at the exact instant of the
     * corruption. Without a tripwire, query the ring via debug-server
     * commands. */
    uint8_t b0 = (uint8_t)(new_val & 0xFF);
    uint8_t b1 = (uint8_t)(new_val >> 8);
    for (int i = 0; i < CPU_WRAM_WATCH_MAX; i++) {
        WramWatch *w = &g_wram_watches[i];
        if (!w->enabled) continue;
        int hit_byte = -1;        /* 0 = low byte, 1 = high byte */
        if (w->ram_offset == ram_off) hit_byte = 0;
        else if (width == 2 && w->ram_offset == ram_off + 1) hit_byte = 1;
        if (hit_byte < 0) continue;
        uint8_t hit_val = (hit_byte == 0) ? b0 : b1;
        /* Always record. extra0 = new value, extra1 = (bank<<8) | hit_byte. */
        uint32_t pc24 = ((uint32_t)cpu->PB << 16); /* low 16 unknown at write site */
        capture(cpu, pc24, CPU_TR_WRAM_WRITE, hit_val,
                (uint16_t)(((uint16_t)bank << 8) | (uint16_t)hit_byte));
        /* Tripwire branch: only if match_value is set AND the value matches.
         * The tripwire is one-shot to avoid stderr spam on tight write
         * loops; recording continues regardless. */
        if (w->match_value && hit_val == w->value) {
            char tag[160];
            snprintf(tag, sizeof(tag),
                     "WRAM-TRIP HIT $%02X:%04X[%d]=$%02X (width=%d) "
                     "at PC ~$%02X:???? slot=%d",
                     bank, addr, hit_byte, hit_val, width, cpu->PB, i);
            cpu_trace_dump_dbpb(tag);
            cpu_trace_dump_recent(tag, 256);
            w->match_value = 0;  /* one-shot tripwire; record stays on */
        }
        /* Don't `return` — multiple watches can cover the same offset
         * (e.g. bytewise + wordwise) and each gets its own recording. */
    }
}

/* Off-rails dump: ONE dump per (tag, distinct hint) — silent for repeat
 * hits of the same kind. The first occurrence captures the chain;
 * additional repeats add nothing. Burning tokens on millions of
 * identical "RomPtr - Invalid 0x570000..0x57FFFF" lines is the
 * exact failure mode the ring buffer was built to avoid. */
#define OFFRAILS_TAG_MAX 16
static struct {
    uint64_t hash;       /* fnv1a(tag) ^ (hint scrambled) */
    uint64_t count;
} s_offrails_seen[OFFRAILS_TAG_MAX];
static int s_offrails_used = 0;

void cpu_trace_offrails(const char *tag, uint32_t hint) {
    /* Cheap key — collision-tolerant, just needs to dedupe spam. */
    uint64_t k = fnv1a(tag ? tag : "");
    k = (k * 0x100000001B3ull) ^ (uint64_t)hint;
    /* Group by (tag, high bytes of hint) so a sweep through
     * \$57:0000-\$57:FFFF dedupes to one dump per page. */
    uint64_t group_k = fnv1a(tag ? tag : "");
    group_k = (group_k * 0x100000001B3ull) ^ ((uint64_t)hint & 0xFFFF0000u);
    for (int i = 0; i < s_offrails_used; i++) {
        if (s_offrails_seen[i].hash == group_k) {
            s_offrails_seen[i].count++;
            return;  /* silent dedup */
        }
    }
    if (s_offrails_used >= OFFRAILS_TAG_MAX) return;  /* table full, silent */
    s_offrails_seen[s_offrails_used].hash = group_k;
    s_offrails_seen[s_offrails_used].count = 1;
    s_offrails_used++;
    char buf[96];
    snprintf(buf, sizeof(buf), "OFF-RAILS [%s] FIRST hint=$%08X",
             tag ? tag : "?", hint);
    cpu_trace_dump_dbpb(buf);
    cpu_trace_dump_recent(buf, 64);
}

/* Public wrapper so debug_server / other TUs can hash names. */
uint32_t fnv1a_extern(const char *s) { return fnv1a(s); }

void cpu_trace_arm_default_watches(void) {
    /* Watch every high bank that SMW should never use as DB at boot.
     * SMW uses DB ∈ {$00, $01, $02, $03, $04, $05, $07, $0C, $0D, $7E,
     * $7F}. Anything outside that — especially the high-ROM-bank
     * mirrors $80-$FF — is poisoning. */
    for (int b = 0x80; b <= 0xFF; b++) cpu_trace_set_db_watch((uint8_t)b, 1);
    /* Also watch the specific garbage values seen in trace runs. */
    for (int b = 0x10; b <= 0x7D; b++) {
        /* Skip known-good DBs. */
        if (b == 0x00 || b == 0x01 || b == 0x02 || b == 0x03 || b == 0x04 ||
            b == 0x05 || b == 0x07 || b == 0x0C || b == 0x0D) continue;
        cpu_trace_set_db_watch((uint8_t)b, 1);
    }
    /* PB should always be $00 in v2 (we set PB explicitly via JSL emit;
     * it should restore on RTL). Watch every NON-zero PB. */
    for (int b = 1; b <= 0xFF; b++) cpu_trace_set_pb_watch((uint8_t)b, 1);
    /* Stack range: $0100-$1FFF (SMW normal native-stack region). */
    cpu_trace_set_s_range_watch(0x0100, 0x1FFF, 1);
    /* Empty fallback stub from bank03.cfg — if reached, codegen has
     * routed past the dispatch HLE. */
    cpu_trace_set_func_watch("GameMode14_InLevel_0086DF");
    /* WRAM recorder for known-investigation addresses. These slots
     * always-on-record every write to the offset; user reads backward.
     * $7E:008c — high byte of GraphicsCompPtr (decompressor bank).
     *   $00:B888 should write $08; if anything else writes here we want
     *   the writer in the ring. No tripwire (match_value=0) so we keep
     *   recording past the first event. */
    cpu_trace_set_wram_watch(0x7E, 0x008C, 1, 0, 0, 1);
    /* $7E:008a/$7E:008b — low/mid bytes of GraphicsCompPtr. The
     * fetch-byte routine INCs $8c only when the low pair wraps, so
     * having the full triplet in the ring lets us reconstruct ptr
     * advancement vs out-of-band corruption. */
    cpu_trace_set_wram_watch(0x7E, 0x008A, 1, 0, 0, 1);
    cpu_trace_set_wram_watch(0x7E, 0x008B, 1, 0, 0, 1);
    /* $7E:0100-$010F — GameMode region. Investigation 2026-04-30:
     * region reads $FF post-boot but BSS is $00. With per-byte WRAM
     * recorders here, the ring captures EVERY write to the region;
     * walk backward from "now" to find the last writer and the value. */
    for (int a = 0x0100; a <= 0x010F; a++) {
        cpu_trace_set_wram_watch(0x7E, (uint16_t)a, 1, 0, 0, 1);
    }
    fprintf(stderr, "[cpu_trace] default watches armed: "
            "DB high banks + odd middle, PB!=0, S out-of-$01XX-$1FFF, "
            "GameMode14_InLevel_0086DF, WRAM recorders on $7E:008A/8B/8C\n");
    /* Arm P.X tripwire at startup so the first 1→0 transition is caught
     * even before TCP attaches. The snapshot doesn't rotate. */
    cpu_trace_arm_px_tripwire();
    fprintf(stderr, "[cpu_trace] P.X tripwire armed (first 1→0 transition caught)\n");
    /* Auto-arm scoped WRAM tripwire on the BG palette buffer
     * $7E:0700-$070F, the first 16 colors of MainPalette/BackgroundColor.
     *
     * Investigation 2026-04-30: oracle-vs-recomp WRAM diff showed
     * recomp's MainPalette ($7E:0703+) has 8 EXTRA bytes inserted
     * between offset 4 and 12 of the buffer ("ce 6a 42 39 08 52 ce 6a"),
     * shifting subsequent bytes forward. Class: dest-pointer increment
     * or loop counter wrong by one iteration in a palette copy loop.
     *
     * We scope to function names containing "Palette" so the BSS-clear
     * loop in InitializeFirst8KBOfRAM / ClearMemory is skipped — we
     * want the FIRST real palette-load writer, not the zero-fill that
     * runs first. Snapshot survives ring rotation; query via
     * `tripwire_get` even if TCP attaches at frame ~1000. */
    /* Narrow target: catch the FIRST write of $FA at $7E:1930.
     * Investigation 2026-04-30: recomp's palette source pointer is
     * 8 bytes off because $7E:1930 = $FA in recomp vs $02 in oracle.
     * The recomp pattern "e0 fe fa" at $192E..$1930 strongly suggests
     * an OVERLAPPING 16-bit STA at $192F storing $FAFE (low $FE at
     * $192F, high $FA at $1930). The value-match tripwire fires on
     * exactly that high-byte event AND on a direct STA $1930 of $FA,
     * so either pattern is captured. The full snapshot includes
     * CPU state, recomp stack, DP $00-$1F, recent block PC, and the
     * trace_idx so the client can pull the last-128 trace events.
     *
     * No scope filter — we don't know which function writes the bad
     * value yet. */
    cpu_trace_arm_scoped_tripwire_v(0x7E, 0x1930, 0x1930, NULL, 0xFA);
    fprintf(stderr, "[cpu_trace] scoped tripwire armed on $7E:1930 (match_val=$FA)\n");
    /* Per-byte recorder on the surrounding region $7E:1925-$1930 so the
     * WRM ring captures the COMPLETE timeline of writes around the
     * corruption site. Walk backward from the trip to see which
     * preceding bytes ($192E, $192F) were written legitimately and
     * which were collateral from the bad 16-bit STA. */
    for (int a = 0x1925; a <= 0x1930; a++) {
        cpu_trace_set_wram_watch(0x7E, (uint16_t)a, 1, 0, 0, 1);
    }
    fflush(stderr);
}

void cpu_trace_clear(void) {
    memset(g_cpu_trace_ring, 0, sizeof(g_cpu_trace_ring));
    memset(g_cpu_dbpb_ring, 0, sizeof(g_cpu_dbpb_ring));
    g_cpu_trace_idx = 0;
    g_cpu_dbpb_idx = 0;
}

static const char *event_name(uint8_t et) {
    switch (et) {
        case CPU_TR_BLOCK:    return "BLOCK";
        case CPU_TR_PHB:      return "PHB";
        case CPU_TR_PLB:      return "PLB";
        case CPU_TR_PHK:      return "PHK";
        case CPU_TR_PLP:      return "PLP";
        case CPU_TR_PHP:      return "PHP";
        case CPU_TR_RTI:      return "RTI";
        case CPU_TR_JSL:      return "JSL";
        case CPU_TR_RTL:      return "RTL";
        case CPU_TR_MVN:      return "MVN";
        case CPU_TR_MVP:      return "MVP";
        case CPU_TR_DB_WRITE: return "DB-WR";
        case CPU_TR_PB_WRITE: return "PB-WR";
        case CPU_TR_FUNC_ENTRY: return "FUNC";
        case CPU_TR_WRAM_WRITE: return "WRAM";
        default:              return "?";
    }
}

void cpu_trace_dump_recent(const char *tag, int n) {
    if (n > CPU_TRACE_RING_LEN) n = CPU_TRACE_RING_LEN;
    if ((uint64_t)n > g_cpu_trace_idx) n = (int)g_cpu_trace_idx;
    fprintf(stderr, "=== %s — last %d trace events ===\n", tag ? tag : "trace", n);
    fprintf(stderr, "  (newest first)\n");
    for (int i = 0; i < n; i++) {
        uint64_t abs_idx = g_cpu_trace_idx - 1 - i;
        int slot = (int)(abs_idx & (CPU_TRACE_RING_LEN - 1));
        CpuTraceEvent *e = &g_cpu_trace_ring[slot];
        fprintf(stderr, "  [%-5s] PC=$%06X DB=%02X PB=%02X A=%04X X=%04X Y=%04X S=%04X "
                        "P=%02X m=%u x=%u",
                event_name(e->event_type), e->pc24, e->DB, e->PB,
                e->A, e->X, e->Y, e->S, e->P, e->M, e->XF);
        switch (e->event_type) {
            case CPU_TR_PLB:
            case CPU_TR_PHB:
            case CPU_TR_DB_WRITE:
                fprintf(stderr, "  DB %02X→%02X", e->extra0, (uint8_t)e->extra1);
                break;
            case CPU_TR_PHK:
            case CPU_TR_PB_WRITE:
            case CPU_TR_JSL:
            case CPU_TR_RTL:
                fprintf(stderr, "  PB %02X→%02X", e->extra0, (uint8_t)e->extra1);
                break;
            case CPU_TR_MVN:
            case CPU_TR_MVP:
                fprintf(stderr, "  src=%02X dst=%02X", e->extra0, (uint8_t)e->extra1);
                break;
            case CPU_TR_FUNC_ENTRY:
                fprintf(stderr, "  hash=%08X", e->native_func_id_or_hash);
                break;
            case CPU_TR_WRAM_WRITE:
                fprintf(stderr, "  bank=%02X byte=%u newval=$%02X",
                        (uint8_t)(e->extra1 >> 8),
                        (uint8_t)(e->extra1 & 0xFF), e->extra0);
                break;
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

void cpu_trace_dump_wram(const char *tag, int scan_n) {
    int total = (int)((g_cpu_trace_idx < CPU_TRACE_RING_LEN) ?
                      g_cpu_trace_idx : CPU_TRACE_RING_LEN);
    if (scan_n <= 0 || scan_n > total) scan_n = total;
    fprintf(stderr, "=== %s — WRAM-WRITE events in last %d ring entries (newest first) ===\n",
            tag ? tag : "wram", scan_n);
    /* Walk backward and remember the most-recent BLOCK / FUNC for each
     * WRAM_WRITE we emit, so the caller can attribute each write to a
     * function context without dumping the entire ring. */
    int wram_count = 0;
    for (int i = 0; i < scan_n && wram_count < 256; i++) {
        uint64_t abs_idx = g_cpu_trace_idx - 1 - (uint64_t)i;
        int slot = (int)(abs_idx & (CPU_TRACE_RING_LEN - 1));
        CpuTraceEvent *e = &g_cpu_trace_ring[slot];
        if (e->event_type != CPU_TR_WRAM_WRITE) continue;
        wram_count++;
        uint8_t hit_bank = (uint8_t)(e->extra1 >> 8);
        uint8_t hit_byte = (uint8_t)(e->extra1 & 0xFF);
        fprintf(stderr,
                "  [WRAM] idx=%llu PC≈$%02X:???? bank=$%02X byte=%u newval=$%02X "
                "DB=%02X PB=%02X A=%04X X=%04X Y=%04X S=%04X D=%04X m=%u x=%u",
                (unsigned long long)abs_idx, (uint8_t)(e->pc24 >> 16),
                hit_bank, hit_byte, e->extra0,
                e->DB, e->PB, e->A, e->X, e->Y, e->S, e->D, e->M, e->XF);
        /* Find the most-recent FUNC entry preceding this write. Walk
         * forward in time from the WRAM event toward newer events first
         * (no — we want what was running, so walk OLDER). Actually want
         * most-recent FUNC AT OR BEFORE this WRAM event. */
        for (int j = 1; j <= 4096; j++) {
            uint64_t prev_abs = abs_idx - (uint64_t)j;
            if (prev_abs >= g_cpu_trace_idx) break;  /* underflow guard */
            int prev_slot = (int)(prev_abs & (CPU_TRACE_RING_LEN - 1));
            CpuTraceEvent *p = &g_cpu_trace_ring[prev_slot];
            if (p->event_type == CPU_TR_FUNC_ENTRY) {
                fprintf(stderr, "  in func@$%06X hash=%08X",
                        p->pc24, p->native_func_id_or_hash);
                break;
            }
        }
        /* Also find most-recent BLOCK at or before this event. */
        for (int j = 1; j <= 256; j++) {
            uint64_t prev_abs = abs_idx - (uint64_t)j;
            if (prev_abs >= g_cpu_trace_idx) break;
            int prev_slot = (int)(prev_abs & (CPU_TRACE_RING_LEN - 1));
            CpuTraceEvent *p = &g_cpu_trace_ring[prev_slot];
            if (p->event_type == CPU_TR_BLOCK) {
                fprintf(stderr, "  block@$%06X", p->pc24);
                break;
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  (%d WRAM-WRITE events found in last %d entries)\n",
            wram_count, scan_n);
    fflush(stderr);
}

void cpu_trace_dump_dbpb(const char *tag) {
    int n = (int)((g_cpu_dbpb_idx < CPU_DBPB_RING_LEN) ? g_cpu_dbpb_idx : CPU_DBPB_RING_LEN);
    fprintf(stderr, "=== %s — last %d DB/PB mutations ===\n", tag ? tag : "dbpb", n);
    for (int i = 0; i < n; i++) {
        uint64_t abs_idx = g_cpu_dbpb_idx - 1 - i;
        int slot = (int)(abs_idx & (CPU_DBPB_RING_LEN - 1));
        CpuDbpbEvent *d = &g_cpu_dbpb_ring[slot];
        fprintf(stderr, "  [%-5s] PC=$%06X %s %02X→%02X (S=$%04X)\n",
                event_name(d->event_type), d->pc24,
                d->reg_id == 0 ? "DB" : "PB",
                d->old_val, d->new_val, d->S);
    }
    fflush(stderr);
}

#endif /* SNESRECOMP_TRACE */
