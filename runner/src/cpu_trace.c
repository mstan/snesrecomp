/* cpu_trace.c — backwards-watcher implementation. See cpu_trace.h. */

#include "cpu_trace.h"

#if SNESRECOMP_TRACE

#include "debug_server.h"
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

CpuTraceEvent *g_cpu_trace_ring = (CpuTraceEvent *)0;
uint64_t      g_cpu_trace_capacity = 0;   /* set by cpu_trace_init; pow2 */
uint64_t      g_cpu_trace_idx = 0;
CpuDbpbEvent  g_cpu_dbpb_ring[CPU_DBPB_RING_LEN];
uint64_t      g_cpu_dbpb_idx = 0;

/* Forward decls — used by boundary auditor below, defined later in this TU. */
static uint32_t fnv1a(const char *s);
extern const char *g_last_recomp_func;
extern const char *g_recomp_stack[];
extern int         g_recomp_stack_top;

/* Round v down to the nearest power of 2 (>= 1<<16). */
static uint64_t round_pow2_down(uint64_t v) {
    if (v == 0) return 0;
    uint64_t p = 1;
    while ((p << 1) && (p << 1) <= v) p <<= 1;
    return p;
}

uint64_t cpu_trace_init(void) {
    uint64_t cap = CPU_TRACE_RING_DEFAULT_ENTRIES;
    const char *env = getenv("SNESRECOMP_CPU_TRACE_RING_ENTRIES");
    if (env && *env) {
        unsigned long long v = strtoull(env, NULL, 0);
        if (v >= (1ULL << 16) && v <= (1ULL << 28)) cap = (uint64_t)v;
    }
    /* Ensure power-of-2 (so `& (cap - 1)` slot math is correct). */
    cap = round_pow2_down(cap);
    if (cap < (1ULL << 16)) cap = (1ULL << 16);
    if (g_cpu_trace_ring) free(g_cpu_trace_ring);
    g_cpu_trace_ring = (CpuTraceEvent *)calloc((size_t)cap, sizeof(CpuTraceEvent));
    g_cpu_trace_capacity = g_cpu_trace_ring ? cap : 0;
    g_cpu_trace_idx = 0;
    fprintf(stderr,
            "[cpu_trace] ring allocated: %llu entries (~%llu MB)%s\n",
            (unsigned long long)g_cpu_trace_capacity,
            (unsigned long long)((g_cpu_trace_capacity * sizeof(CpuTraceEvent)) >> 20),
            g_cpu_trace_ring ? "" : " — ALLOC FAILED");
    /* Allocate the boundary auditor ring alongside cpu_trace. Same
     * heap-alloc / pow2 / env-tunable shape; sized independently. */
    boundary_audit_init();
    /* Allocate the GM14 per-tick player-trace ring. Same shape; sized
     * for ~18 min of in-level play at 60Hz. */
    cpu_trace_gm14_init();
    return g_cpu_trace_capacity;
}
uint8_t       g_db_watch_set = 0;
uint32_t      g_db_watch_bits[8] = {0};
ScopedTripwire g_scoped_tripwire = {0};
PxTripwire     g_px_tripwire = {0};

/* ── Boundary auditor ────────────────────────────────────────────────────
 *
 * Per generated-function entry/exit ring. Hooked into RecompStackPush /
 * RecompStackPop so coverage is automatic for every recompiled function.
 * Pairing across entry/exit is kept in the parallel `s_active_seq` stack
 * (one entry per active call). Sized to match RECOMP_STACK_DEPTH.
 */
BoundaryEvent *g_boundary_ring = (BoundaryEvent *)0;
uint64_t       g_boundary_capacity = 0;
uint64_t       g_boundary_idx = 0;
/* When set non-zero, all subsequent boundary events are dropped. Used
 * by tripwires to FREEZE the ring at fire time so the captured window
 * survives post-trip activity and can be walked from seq=0 to find
 * the first runtime imbalance. */
uint8_t        g_boundary_frozen = 0;

/* Deliberate inspection-freeze. Distinct from g_boundary_frozen: when set,
 * the always-on cpu_trace ring (capture / func_entry / wram_write) ALSO
 * stops, so a window can be inspected at leisure without the runaway game
 * overwriting it. Armed via SNESRECOMP_FREEZE_AT_FRAME=<N> (from boot, no
 * arming race) or the `freeze_at_frame` / `freeze_now` TCP commands.
 * g_freeze_at_frame > 0 => freeze ALL rings the first cpu_trace_block at
 * snes_frame_counter >= g_freeze_at_frame. -1 = disabled. */
uint8_t        g_freeze_capture = 0;
int            g_freeze_at_frame = -1;

#define BOUNDARY_ACTIVE_DEPTH 64  /* matches RECOMP_STACK_DEPTH */
typedef struct ActiveCall {
    uint64_t entry_seq;
    uint16_t entry_S;
} ActiveCall;
static ActiveCall s_active[BOUNDARY_ACTIVE_DEPTH];
static int        s_active_top = 0;

/* Pending NLR exit-kind flag — set by codegen via
 * cpu_trace_mark_nlr_exit() right before a SKIP-propagation return,
 * consumed (and reset) by boundary_audit_record_exit. Lets the
 * stack-drift tripwire ignore intentional NLR-cascade imbalances. */
static uint8_t s_pending_exit_kind = BD_EXIT_KIND_NORMAL;

void cpu_trace_mark_nlr_exit(uint8_t kind) {
    s_pending_exit_kind = kind;
}

uint64_t boundary_audit_init(void) {
    uint64_t cap = BOUNDARY_RING_DEFAULT_ENTRIES;
    const char *env = getenv("SNESRECOMP_BOUNDARY_RING_ENTRIES");
    if (env && *env) {
        unsigned long long v = strtoull(env, NULL, 0);
        if (v >= (1ULL << 14) && v <= (1ULL << 24)) cap = (uint64_t)v;
    }
    cap = round_pow2_down(cap);
    if (cap < (1ULL << 14)) cap = (1ULL << 14);
    if (g_boundary_ring) free(g_boundary_ring);
    g_boundary_ring = (BoundaryEvent *)calloc((size_t)cap, sizeof(BoundaryEvent));
    g_boundary_capacity = g_boundary_ring ? cap : 0;
    g_boundary_idx = 0;
    s_active_top = 0;
    fprintf(stderr,
            "[boundary_audit] ring allocated: %llu entries (~%llu MB)%s\n",
            (unsigned long long)g_boundary_capacity,
            (unsigned long long)((g_boundary_capacity * sizeof(BoundaryEvent)) >> 20),
            g_boundary_ring ? "" : " — ALLOC FAILED");
    return g_boundary_capacity;
}

/* Capture register state from g_cpu into a BoundaryEvent. */
static void boundary_fill_state(BoundaryEvent *be) {
    extern CpuState g_cpu;  /* defined in common_rtl.c */
    be->A = g_cpu.A; be->X = g_cpu.X; be->Y = g_cpu.Y;
    be->S = g_cpu.S; be->D = g_cpu.D;
    be->DB = g_cpu.DB; be->PB = g_cpu.PB; be->P = g_cpu.P;
    be->m_flag = g_cpu.m_flag; be->x_flag = g_cpu.x_flag;
}

void boundary_audit_record_entry(const char *name) {
    if (!g_boundary_ring || !g_boundary_capacity) return;
    if (g_boundary_frozen) return;  /* tripwire froze the ring */
    extern int snes_frame_counter;
    uint64_t seq = g_boundary_idx++;
    int slot = (int)(seq & (g_boundary_capacity - 1));
    BoundaryEvent *be = &g_boundary_ring[slot];
    be->seq = seq;
    be->entry_seq = 0;
    be->frame = snes_frame_counter;
    be->name_hash = name ? fnv1a(name) : 0;
    boundary_fill_state(be);
    be->kind = BD_ENTRY;
    be->stack_depth = (uint8_t)(g_recomp_stack_top > 255 ? 255 : g_recomp_stack_top);
    be->exit_kind = BD_EXIT_KIND_NORMAL;  /* unused on ENTRY */
    be->pad[0] = be->pad[1] = 0;
    if (name) {
        strncpy(be->name, name, BOUNDARY_NAME_LEN - 1);
        be->name[BOUNDARY_NAME_LEN - 1] = 0;
    } else {
        be->name[0] = 0;
    }
    /* Track entry seq + entry_S on the parallel call-stack so the
     * matching EXIT can reference both. Saturate at
     * BOUNDARY_ACTIVE_DEPTH; if the call stack runs deeper, EXIT
     * events still get written — they just carry entry_seq=0
     * (unpaired). */
    if (s_active_top < BOUNDARY_ACTIVE_DEPTH) {
        s_active[s_active_top].entry_seq = seq;
        s_active[s_active_top].entry_S = be->S;
        s_active_top++;
    }
    /* GM14 player-trace hook: filter on GameMode14_InLevel_00C47E_M1X1 by
     * pointer-cached name match. Records one row per ENTRY into the
     * always-on per-tick ring. */
    cpu_trace_gm14_maybe_record(name, GM14_KIND_ENTRY);
}

void boundary_audit_record_exit(const char *name) {
    if (!g_boundary_ring || !g_boundary_capacity) return;
    if (g_boundary_frozen) return;  /* tripwire froze the ring */
    extern int snes_frame_counter;
    uint64_t seq = g_boundary_idx++;
    int slot = (int)(seq & (g_boundary_capacity - 1));
    BoundaryEvent *be = &g_boundary_ring[slot];
    be->seq = seq;
    /* Pop the matching entry seq + entry_S off the parallel stack.
     * If empty (over-popped) leave entry_seq=0 — that itself is
     * signal: an unbalanced exit. */
    uint16_t entry_S = 0;
    int paired = 0;
    if (s_active_top > 0) {
        s_active_top--;
        be->entry_seq = s_active[s_active_top].entry_seq;
        entry_S = s_active[s_active_top].entry_S;
        paired = 1;
    } else {
        be->entry_seq = 0;
    }
    be->frame = snes_frame_counter;
    be->name_hash = name ? fnv1a(name) : 0;
    boundary_fill_state(be);
    be->kind = BD_EXIT;
    be->stack_depth = (uint8_t)(g_recomp_stack_top > 255 ? 255 : g_recomp_stack_top);
    /* Consume + reset the pending NLR exit-kind flag. */
    be->exit_kind = s_pending_exit_kind;
    s_pending_exit_kind = BD_EXIT_KIND_NORMAL;
    be->pad[0] = be->pad[1] = 0;
    if (name) {
        strncpy(be->name, name, BOUNDARY_NAME_LEN - 1);
        be->name[BOUNDARY_NAME_LEN - 1] = 0;
    } else {
        be->name[0] = 0;
    }
    /* Stack-drift tripwire — fires on the FIRST function exit after
     * frame_min where exit_S != entry_S AND exit_kind == NORMAL.
     * SKIP-propagation and NLR-primary exits are LEGITIMATELY
     * unbalanced under the asm "skip caller" semantic and must be
     * ignored; only NORMAL exits with mismatched S indicate a bug. */
    if (paired) {
        cpu_trace_stack_drift_check(entry_S, be->S, be->entry_seq, seq,
                                    be->name, be->exit_kind);
    }
    /* GM14 player-trace hook (EXIT side). Same name filter as ENTRY
     * via cpu_trace_gm14_maybe_record. */
    cpu_trace_gm14_maybe_record(name, GM14_KIND_EXIT);
}

/* ── GM14 (in-level) per-tick player-state trace ring ──────────────────
 *
 * Filter: name == "GameMode14_InLevel_00C47E_M1X1". The first time we
 * see that exact pointer (or strcmp-match) we cache it; subsequent
 * compares are pointer-equality fast-path because gen-emitted string
 * literals are stable across the run. Reads field bytes directly from
 * g_ram[] so it sees writes from gen + hand-bodies.
 */
extern uint8_t g_ram[];
Gm14TickRow *g_gm14_trace_ring = (Gm14TickRow *)0;
uint64_t     g_gm14_trace_capacity = 0;
uint64_t     g_gm14_trace_idx = 0;
uint64_t     g_gm14_tick_ordinal = 0;

static const char  GM14_TARGET_NAME[] = "GameMode14_InLevel_00C47E_M1X1";
static const char *g_gm14_cached_ptr = NULL;

uint64_t cpu_trace_gm14_init(void) {
    uint64_t cap = GM14_TRACE_DEFAULT_ENTRIES;
    const char *env = getenv("SNESRECOMP_GM14_TRACE_ENTRIES");
    if (env && *env) {
        unsigned long long v = strtoull(env, NULL, 0);
        if (v >= (1ULL << 14) && v <= (1ULL << 22)) cap = (uint64_t)v;
    }
    cap = round_pow2_down(cap);
    if (cap < (1ULL << 14)) cap = (1ULL << 14);
    if (g_gm14_trace_ring) free(g_gm14_trace_ring);
    g_gm14_trace_ring = (Gm14TickRow *)calloc((size_t)cap, sizeof(Gm14TickRow));
    g_gm14_trace_capacity = g_gm14_trace_ring ? cap : 0;
    g_gm14_trace_idx = 0;
    g_gm14_tick_ordinal = 0;
    g_gm14_cached_ptr = NULL;
    fprintf(stderr,
            "[gm14_trace] ring allocated: %llu entries (~%llu MB)%s\n",
            (unsigned long long)g_gm14_trace_capacity,
            (unsigned long long)((g_gm14_trace_capacity * sizeof(Gm14TickRow)) >> 20),
            g_gm14_trace_ring ? "" : " — ALLOC FAILED");
    return g_gm14_trace_capacity;
}

void cpu_trace_gm14_clear(void) {
    g_gm14_trace_idx = 0;
    g_gm14_tick_ordinal = 0;
    if (g_gm14_trace_ring && g_gm14_trace_capacity) {
        memset(g_gm14_trace_ring, 0,
               (size_t)g_gm14_trace_capacity * sizeof(Gm14TickRow));
    }
}

static inline int gm14_name_matches(const char *name) {
    if (!name) return 0;
    if (name == g_gm14_cached_ptr) return 1;
    if (g_gm14_cached_ptr == NULL && strcmp(name, GM14_TARGET_NAME) == 0) {
        g_gm14_cached_ptr = name;
        return 1;
    }
    return 0;
}

void cpu_trace_gm14_maybe_record(const char *func_name, uint8_t kind) {
    if (!g_gm14_trace_ring || !g_gm14_trace_capacity) return;
    if (!gm14_name_matches(func_name)) return;
    if (kind == GM14_KIND_ENTRY) g_gm14_tick_ordinal++;
    extern int snes_frame_counter;
    uint64_t slot = g_gm14_trace_idx & (g_gm14_trace_capacity - 1);
    Gm14TickRow *r = &g_gm14_trace_ring[slot];
    r->tick_ordinal = g_gm14_tick_ordinal;
    r->host_frame   = snes_frame_counter;
    r->kind         = kind;
    r->gamemode     = g_ram[0x0100];
    r->in_15        = g_ram[0x0015];
    r->in_16        = g_ram[0x0016];
    r->in_17        = g_ram[0x0017];
    r->in_18        = g_ram[0x0018];
    r->st_71        = g_ram[0x0071];
    r->st_77        = g_ram[0x0077];
    r->xspeed       = (int8_t)g_ram[0x007B];
    r->yspeed       = (int8_t)g_ram[0x007D];
    r->pos_x        = (uint16_t)(g_ram[0x0094] | ((uint16_t)g_ram[0x0095] << 8));
    r->pos_y        = (uint16_t)(g_ram[0x0096] | ((uint16_t)g_ram[0x0097] << 8));
    r->yoshi_187A   = g_ram[0x187A];
    r->yoshi_18E2   = g_ram[0x18E2];
    r->yoshi_1888   = g_ram[0x1888];
    r->scroll_1A    = g_ram[0x001A];
    r->scroll_1C    = g_ram[0x001C];
    r->pad0         = 0;
    r->cam_1462     = (uint16_t)(g_ram[0x1462] | ((uint16_t)g_ram[0x1463] << 8));
    r->cam_1464     = (uint16_t)(g_ram[0x1464] | ((uint16_t)g_ram[0x1465] << 8));
    r->spr9_status  = g_ram[0x14C8 + 9];
    r->spr9_number  = g_ram[0x009E + 9];
    r->spr9_x       = (uint16_t)(g_ram[0x00E4 + 9] | ((uint16_t)g_ram[0x14E0 + 9] << 8));
    r->spr9_y       = (uint16_t)(g_ram[0x00D8 + 9] | ((uint16_t)g_ram[0x14D4 + 9] << 8));
    g_gm14_trace_idx++;
}

/* ── DB→<value> one-shot tripwire ──────────────────────────────────────
 *
 * Captures full state + last 256 boundary events + last 64 dbpb events
 * the FIRST time cpu->DB transitions FROM != target_db TO target_db.
 * Auto-arms target_db=$C0 in cpu_trace_arm_default_watches.
 */
DbTripwire g_db_tripwire = {0};

void cpu_trace_arm_db_tripwire(uint8_t target_db) {
    memset(&g_db_tripwire, 0, sizeof(g_db_tripwire));
    g_db_tripwire.armed = 1;
    g_db_tripwire.target_db = target_db;
}

void cpu_trace_disarm_db_tripwire(void) {
    g_db_tripwire.armed = 0;
}

/* Snapshot the last `want` boundary events into the tripwire. Walks
 * backward from g_boundary_idx (which is the next-to-write slot). */
static void db_tripwire_snapshot_boundary(int want) {
    if (want > DB_TRIP_BOUNDARY_HISTORY) want = DB_TRIP_BOUNDARY_HISTORY;
    int avail = (int)(g_boundary_idx < (uint64_t)want ? g_boundary_idx : (uint64_t)want);
    if (avail > (int)g_boundary_capacity) avail = (int)g_boundary_capacity;
    g_db_tripwire.bd_count = avail;
    /* Newest-first: bd_history[0] = most-recent boundary event. */
    for (int i = 0; i < avail; i++) {
        uint64_t abs = g_boundary_idx - 1 - (uint64_t)i;
        int slot = (int)(abs & (g_boundary_capacity - 1));
        g_db_tripwire.bd_history[i] = g_boundary_ring[slot];
    }
}

/* Snapshot the last `want` dbpb events into the tripwire. */
static void db_tripwire_snapshot_dbpb(int want) {
    if (want > DB_TRIP_DBPB_HISTORY) want = DB_TRIP_DBPB_HISTORY;
    int avail = (int)(g_cpu_dbpb_idx < (uint64_t)want ? g_cpu_dbpb_idx : (uint64_t)want);
    if (avail > CPU_DBPB_RING_LEN) avail = CPU_DBPB_RING_LEN;
    g_db_tripwire.dbpb_count = avail;
    for (int i = 0; i < avail; i++) {
        uint64_t abs = g_cpu_dbpb_idx - 1 - (uint64_t)i;
        int slot = (int)(abs & (CPU_DBPB_RING_LEN - 1));
        g_db_tripwire.dbpb_history[i] = g_cpu_dbpb_ring[slot];
    }
}

void cpu_trace_db_tripwire_check(CpuState *cpu, uint32_t pc24,
                                 uint8_t old_db, uint8_t new_db,
                                 uint8_t event_type) {
    DbTripwire *t = &g_db_tripwire;
    if (!t->armed || t->triggered) return;
    if (new_db != t->target_db) return;
    if (old_db == t->target_db) return;  /* already at target — not a transition */

    extern int snes_frame_counter;
    t->triggered = 1;
    t->frame = snes_frame_counter;
    t->trip_pc24 = pc24;
    t->old_db = old_db;
    t->new_db = new_db;
    t->trip_event_type = event_type;
    t->trip_boundary_seq = g_boundary_idx;
    t->trip_trace_idx = g_cpu_trace_idx;

    if (cpu) {
        t->A = cpu->A; t->X = cpu->X; t->Y = cpu->Y;
        t->S = cpu->S; t->D = cpu->D;
        t->DB = cpu->DB; t->PB = cpu->PB; t->P = cpu->P;
        t->m_flag = cpu->m_flag; t->x_flag = cpu->x_flag;
        t->e_flag = cpu->emulation;
    }

    if (g_last_recomp_func) {
        strncpy(t->last_func, g_last_recomp_func, DB_TRIP_FUNC_LEN - 1);
        t->last_func[DB_TRIP_FUNC_LEN - 1] = 0;
    }

    int depth = g_recomp_stack_top;
    if (depth > DB_TRIP_STACK_DEPTH) depth = DB_TRIP_STACK_DEPTH;
    t->stack_depth = depth;
    int skip = g_recomp_stack_top - depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) {
            strncpy(t->stack[i], p, DB_TRIP_FUNC_LEN - 1);
            t->stack[i][DB_TRIP_FUNC_LEN - 1] = 0;
        } else {
            t->stack[i][0] = 0;
        }
    }

    db_tripwire_snapshot_boundary(DB_TRIP_BOUNDARY_HISTORY);
    db_tripwire_snapshot_dbpb(DB_TRIP_DBPB_HISTORY);
    /* Freeze the boundary ring so post-trip activity can't overwrite
     * the seq=0..trip_seq window. Probes that walk forward from seq=0
     * looking for the first runtime imbalance need this — without it,
     * a 1M-event ring fills in seconds and the trip context is gone. */
    g_boundary_frozen = 1;

    fprintf(stderr,
            "[db_tripwire] FIRED frame=%d DB %02X -> %02X at pc=$%06X "
            "func=%s S=$%04X bd_seq=%llu (boundary history captured: %d events)\n",
            t->frame, old_db, new_db, pc24,
            t->last_func[0] ? t->last_func : "(none)",
            (unsigned)t->S,
            (unsigned long long)t->trip_boundary_seq,
            (int)t->bd_count);
    fflush(stderr);
}

/* ── Stack-drift tripwire ──────────────────────────────────────────── */
StackDriftTripwire g_stack_drift_tripwire = {0};

void cpu_trace_arm_stack_drift_tripwire(int32_t frame_min) {
    memset(&g_stack_drift_tripwire, 0, sizeof(g_stack_drift_tripwire));
    g_stack_drift_tripwire.armed = 1;
    g_stack_drift_tripwire.frame_min = frame_min;
}

void cpu_trace_disarm_stack_drift_tripwire(void) {
    g_stack_drift_tripwire.armed = 0;
}

static void stack_drift_tripwire_snapshot_boundary(int want) {
    if (want > STACK_DRIFT_TRIP_BD_HISTORY) want = STACK_DRIFT_TRIP_BD_HISTORY;
    int avail = (int)(g_boundary_idx < (uint64_t)want ? g_boundary_idx : (uint64_t)want);
    if (avail > (int)g_boundary_capacity) avail = (int)g_boundary_capacity;
    g_stack_drift_tripwire.bd_count = avail;
    /* Newest-first. */
    for (int i = 0; i < avail; i++) {
        uint64_t abs = g_boundary_idx - 1 - (uint64_t)i;
        int slot = (int)(abs & (g_boundary_capacity - 1));
        g_stack_drift_tripwire.bd_history[i] = g_boundary_ring[slot];
    }
}

void cpu_trace_stack_drift_check(uint16_t entry_S, uint16_t exit_S,
                                 uint64_t entry_seq, uint64_t exit_seq,
                                 const char *func_name,
                                 uint8_t exit_kind) {
    StackDriftTripwire *t = &g_stack_drift_tripwire;
    if (!t->armed || t->triggered) return;
    /* Only NORMAL exits matter — SKIP-propagation cascades are
     * legitimately unbalanced under the asm "skip caller" semantic. */
    if (exit_kind != BD_EXIT_KIND_NORMAL) return;
    /* Frame gate so boot prolog doesn't trigger spurious imbalances. */
    extern int snes_frame_counter;
    if (snes_frame_counter < t->frame_min) return;
    /* The actual invariant: function exit must preserve S. */
    if (entry_S == exit_S) return;

    t->triggered = 1;
    t->frame = snes_frame_counter;
    t->s_delta = (int32_t)exit_S - (int32_t)entry_S;
    t->entry_S = entry_S;
    t->exit_S = exit_S;
    t->entry_seq = entry_seq;
    t->exit_seq = exit_seq;
    if (func_name) {
        strncpy(t->func_name, func_name, STACK_DRIFT_TRIP_FUNC_LEN - 1);
        t->func_name[STACK_DRIFT_TRIP_FUNC_LEN - 1] = 0;
    }

    extern CpuState g_cpu;
    t->A = g_cpu.A; t->X = g_cpu.X; t->Y = g_cpu.Y;
    t->S = g_cpu.S; t->D = g_cpu.D;
    t->DB = g_cpu.DB; t->PB = g_cpu.PB; t->P = g_cpu.P;
    t->m_flag = g_cpu.m_flag; t->x_flag = g_cpu.x_flag;
    t->e_flag = g_cpu.emulation;

    int depth = g_recomp_stack_top;
    if (depth > STACK_DRIFT_TRIP_STACK_DEPTH) depth = STACK_DRIFT_TRIP_STACK_DEPTH;
    t->stack_depth = depth;
    int skip = g_recomp_stack_top - depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) {
            strncpy(t->stack[i], p, STACK_DRIFT_TRIP_FUNC_LEN - 1);
            t->stack[i][STACK_DRIFT_TRIP_FUNC_LEN - 1] = 0;
        } else {
            t->stack[i][0] = 0;
        }
    }

    stack_drift_tripwire_snapshot_boundary(STACK_DRIFT_TRIP_BD_HISTORY);
    g_boundary_frozen = 1;

    fprintf(stderr,
            "[stack_drift] FIRED frame=%d func=%s S_delta=%+d "
            "(entry=$%04X exit=$%04X) bd_count=%d\n",
            t->frame, t->func_name, t->s_delta,
            (unsigned)t->entry_S, (unsigned)t->exit_S,
            (int)t->bd_count);
    fflush(stderr);
}

/* ── Static M/X claim verifier ────────────────────────────────────────
 *
 * Accumulating table; one record per distinct (func_name, runtime_m,
 * runtime_x) key. First-seen state is captured per key, subsequent
 * hits only bump the counter. This is always-on observability — no
 * "triggered" latch, no ring-buffer freeze. */
MxClaimTable g_mx_claim_table = {0};

void cpu_trace_arm_mx_claim_check(void) {
    memset(&g_mx_claim_table, 0, sizeof(g_mx_claim_table));
    for (int i = 0; i < MX_CLAIM_HASH_SIZE; i++) {
        g_mx_claim_table.hash[i] = MX_CLAIM_HASH_EMPTY;
    }
    g_mx_claim_table.armed = 1;
}

void cpu_trace_disarm_mx_claim_check(void) {
    g_mx_claim_table.armed = 0;
}

/* Parse trailing `_M{0|1}X{0|1}` (exactly 5 chars at end of name).
 * Returns 1 if parsed; *out_m and *out_x set accordingly. Returns 0
 * if the suffix is absent (e.g., hand-written entry-point shims, or
 * names from older variants). */
static int _parse_variant_suffix(const char *name, uint8_t *out_m,
                                 uint8_t *out_x) {
    if (!name) return 0;
    size_t len = strlen(name);
    if (len < 5) return 0;
    const char *s = name + len - 5;
    if (s[0] != '_' || s[1] != 'M' || s[3] != 'X') return 0;
    if (s[2] != '0' && s[2] != '1') return 0;
    if (s[4] != '0' && s[4] != '1') return 0;
    *out_m = (uint8_t)(s[2] == '1' ? 1 : 0);
    *out_x = (uint8_t)(s[4] == '1' ? 1 : 0);
    return 1;
}

/* FNV-1a over (func_name || runtime_m || runtime_x). */
static uint32_t _mx_claim_hash(const char *name,
                               uint8_t runtime_m, uint8_t runtime_x) {
    uint32_t h = 0x811c9dc5u;
    if (name) {
        for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
            h ^= *p;
            h *= 0x01000193u;
        }
    }
    h ^= (uint32_t)(((runtime_m & 1) << 1) | (runtime_x & 1));
    h *= 0x01000193u;
    return h;
}

/* Find existing record matching (name, runtime_m, runtime_x), or
 * allocate a new one. Returns NULL on table overflow (>= 256 keys or
 * full probe chain) — caller increments overflow_count. */
static MxClaimRecord *_mx_claim_lookup_or_alloc(const char *name,
                                                uint8_t runtime_m,
                                                uint8_t runtime_x) {
    MxClaimTable *t = &g_mx_claim_table;
    uint32_t mask = MX_CLAIM_HASH_SIZE - 1;
    uint32_t pos = _mx_claim_hash(name, runtime_m, runtime_x) & mask;
    const char *key_name = name ? name : "";

    for (uint32_t probe = 0; probe < MX_CLAIM_HASH_SIZE; probe++) {
        uint16_t idx = t->hash[pos];
        if (idx == MX_CLAIM_HASH_EMPTY) {
            if (t->record_count >= MX_CLAIM_RECORD_CAP) return NULL;
            uint32_t new_idx = t->record_count++;
            t->hash[pos] = (uint16_t)new_idx;
            MxClaimRecord *r = &t->records[new_idx];
            r->used = 1;
            r->runtime_m = runtime_m;
            r->runtime_x = runtime_x;
            strncpy(r->func_name, key_name, MX_CLAIM_TRIP_FUNC_LEN - 1);
            r->func_name[MX_CLAIM_TRIP_FUNC_LEN - 1] = 0;
            return r;
        }
        MxClaimRecord *r = &t->records[idx];
        if (r->runtime_m == runtime_m && r->runtime_x == runtime_x &&
            strncmp(r->func_name, key_name, MX_CLAIM_TRIP_FUNC_LEN) == 0) {
            return r;
        }
        pos = (pos + 1) & mask;
    }
    return NULL;
}

int cpu_trace_mx_claim_check(CpuState *cpu, uint32_t pc24, const char *name) {
    MxClaimTable *t = &g_mx_claim_table;
    if (!t->armed) return 1;

    uint8_t claimed_m = 0, claimed_x = 0;
    if (!_parse_variant_suffix(name, &claimed_m, &claimed_x)) {
        /* No suffix to compare against — neither a pass nor a fail. */
        return 1;
    }

    uint8_t runtime_m = (uint8_t)(cpu->m_flag & 1);
    uint8_t runtime_x = (uint8_t)(cpu->x_flag & 1);
    if (runtime_m == claimed_m && runtime_x == claimed_x) {
        return 1;  /* claim matches reality */
    }

    /* Mismatch — accumulate. */
    t->total_hits++;
    MxClaimRecord *r = _mx_claim_lookup_or_alloc(name, runtime_m, runtime_x);
    if (!r) {
        t->overflow_count++;
        return 0;
    }

    if (r->hit_count == 0) {
        /* First-seen — capture full state + stack snapshot. */
        extern int snes_frame_counter;
        r->first_frame = snes_frame_counter;
        r->first_pc24 = pc24;
        r->claimed_m = claimed_m;
        r->claimed_x = claimed_x;

        r->A = cpu->A; r->X = cpu->X; r->Y = cpu->Y;
        r->S = cpu->S; r->D = cpu->D;
        r->DB = cpu->DB; r->PB = cpu->PB; r->P = cpu->P;
        r->e_flag = cpu->emulation;

        int depth = g_recomp_stack_top;
        if (depth > MX_CLAIM_TRIP_STACK_DEPTH) depth = MX_CLAIM_TRIP_STACK_DEPTH;
        r->stack_depth = depth;
        int skip = g_recomp_stack_top - depth;
        for (int i = 0; i < depth; i++) {
            const char *p = g_recomp_stack[skip + i];
            if (p) {
                strncpy(r->stack[i], p, MX_CLAIM_TRIP_FUNC_LEN - 1);
                r->stack[i][MX_CLAIM_TRIP_FUNC_LEN - 1] = 0;
            } else {
                r->stack[i][0] = 0;
            }
        }

        fprintf(stderr,
                "[mx_claim] FIRST-SEEN frame=%d func=%s pc=$%06X "
                "claimed=(M%uX%u) runtime=(M%uX%u) (record %u/%u)\n",
                r->first_frame, r->func_name, (unsigned)r->first_pc24,
                (unsigned)claimed_m, (unsigned)claimed_x,
                (unsigned)runtime_m, (unsigned)runtime_x,
                (unsigned)t->record_count, (unsigned)MX_CLAIM_RECORD_CAP);
        fflush(stderr);
    }
    r->hit_count++;
    return 0;
}

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

/* Unconditionally count P-mutation emit events (SEP/REP/PHP/PLP/XCE/
 * RTI). Used by the async-m/x-flag-write tripwire below to distinguish
 * legitimate (decoder-emitted) flag changes from unexpected (async)
 * ones. Incremented even when g_px_tripwire is disarmed so the async
 * tripwire's "did we just see an emitted P-mutation" check stays
 * accurate. */
uint64_t g_px_mutation_count = 0;
MxAsyncTrip g_mx_async_trip;

void cpu_trace_arm_mx_async_check(void) {
    memset(&g_mx_async_trip, 0, sizeof(g_mx_async_trip));
    g_mx_async_trip.armed = 1;
}

void cpu_trace_disarm_mx_async_check(void) {
    g_mx_async_trip.armed = 0;
}

int cpu_trace_mx_async_check(CpuState *cpu, uint32_t pc24) {
    MxAsyncTrip *t = &g_mx_async_trip;
    if (!t->armed || t->triggered) return 1;

    uint8_t  cur_m     = (uint8_t)(cpu->m_flag & 1);
    uint8_t  cur_x     = (uint8_t)(cpu->x_flag & 1);
    uint64_t cur_count = g_px_mutation_count;

    if (!t->initialized) {
        t->last_m        = cur_m;
        t->last_x        = cur_x;
        t->last_px_count = cur_count;
        t->initialized   = 1;
        return 1;
    }

    int flags_changed = (cur_m != t->last_m) || (cur_x != t->last_x);
    int px_unchanged  = (cur_count == t->last_px_count);

    if (flags_changed && px_unchanged) {
        /* Latch one-shot. */
        t->triggered        = 1;
        extern int snes_frame_counter;
        t->frame            = snes_frame_counter;
        t->block_pc24       = pc24;
        t->prev_m           = t->last_m;
        t->prev_x           = t->last_x;
        t->new_m            = cur_m;
        t->new_x            = cur_x;
        t->px_count_at_trip = cur_count;

        if (g_recomp_stack_top > 0 && g_recomp_stack[g_recomp_stack_top - 1]) {
            strncpy(t->func_name, g_recomp_stack[g_recomp_stack_top - 1],
                    MX_CLAIM_TRIP_FUNC_LEN - 1);
            t->func_name[MX_CLAIM_TRIP_FUNC_LEN - 1] = 0;
        }
        int depth = g_recomp_stack_top;
        if (depth > MX_CLAIM_TRIP_STACK_DEPTH) depth = MX_CLAIM_TRIP_STACK_DEPTH;
        t->stack_depth = depth;
        int skip = g_recomp_stack_top - depth;
        for (int i = 0; i < depth; i++) {
            const char *p = g_recomp_stack[skip + i];
            if (p) {
                strncpy(t->stack[i], p, MX_CLAIM_TRIP_FUNC_LEN - 1);
                t->stack[i][MX_CLAIM_TRIP_FUNC_LEN - 1] = 0;
            } else {
                t->stack[i][0] = 0;
            }
        }

        fprintf(stderr,
                "[mx_async] FIRED frame=%d block=$%06X func=%s "
                "flags (%u,%u)->(%u,%u) px_count=%llu (no emit between checkpoints)\n",
                t->frame, t->block_pc24, t->func_name,
                t->prev_m, t->prev_x, t->new_m, t->new_x,
                (unsigned long long)cur_count);
        fflush(stderr);
        /* Don't update the snapshot — we want the trip data to remain
         * stable for later queries. */
        return 0;
    }

    /* No trip — update the snapshot for the next checkpoint. */
    t->last_m        = cur_m;
    t->last_x        = cur_x;
    t->last_px_count = cur_count;
    return 1;
}

void cpu_trace_px_record(CpuState *cpu, uint32_t pc24, uint8_t source_kind,
                         uint8_t old_p, uint8_t new_p) {
    /* Count this emit event unconditionally — the async-mx-write
     * tripwire compares this count across block hooks to distinguish
     * legitimate (emitted) flag changes from unexpected ones. */
    g_px_mutation_count++;

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
        int slot = (int)(abs_idx & (g_cpu_trace_capacity - 1));
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

/* When the stack-drift tripwire fires, freeze the cpu_trace ring so
 * the events leading up to the trip survive subsequent activity. The
 * boundary ring is already frozen by cpu_trace_stack_drift_check; this
 * mirrors that for cpu_trace so retrospective probes have full context. */
extern StackDriftTripwire g_stack_drift_tripwire;
static void capture(CpuState *cpu, uint32_t pc24, uint8_t event_type,
                    uint8_t extra0, uint16_t extra1) {
    if (g_freeze_capture) return;  /* deliberate inspection-freeze */
    /* Ring is always-on. Tripwires snapshot via boundary_get / their own
     * struct copies; freezing capture() here destroys observability for
     * any investigation that runs *after* a tripwire fires (boundary
     * ring + WRAM watches + STACK_OP events all stop), so we never want
     * the live cpu_trace ring frozen. See CLAUDE.md global rules
     * "always-on ring buffers". */
    int slot = (int)(g_cpu_trace_idx++ & (g_cpu_trace_capacity - 1));
    CpuTraceEvent *e = &g_cpu_trace_ring[slot];
    e->pc24 = pc24;
    e->frame = snes_frame_counter;
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
    /* B2: zero-default the WRAM_WRITE-specific fields. WRAM_WRITE call
     * sites overwrite these immediately after capture() returns. */
    e->bank = 0;
    e->width = 0;
    e->addr16 = 0;
    e->old_value = 0;
    e->new_value = 0;
}

/* Forward decl — phantom-PC trap check defined later in the file. */
static inline void phantom_check(CpuState *cpu, uint32_t pc24);

/* ── Block-keyed sampler ────────────────────────────────────────────── */
BlockWatch g_block_watches[BLOCK_WATCH_MAX] = {0};
uint8_t    g_block_watch_any = 0;

void cpu_trace_block_watch_arm(uint32_t pc24,
                                const int32_t *ram_offsets,
                                int n_addrs,
                                int max_hits) {
    if (n_addrs < 0) n_addrs = 0;
    if (n_addrs > BLOCK_WATCH_ADDRS_MAX) n_addrs = BLOCK_WATCH_ADDRS_MAX;
    if (max_hits < 1) max_hits = 1;
    if (max_hits > BLOCK_WATCH_HITS_MAX) max_hits = BLOCK_WATCH_HITS_MAX;
    int slot = -1;
    /* Reuse if same pc24 already armed (re-arm clears previous hits). */
    for (int i = 0; i < BLOCK_WATCH_MAX; i++) {
        if (g_block_watches[i].enabled && g_block_watches[i].pc24 == pc24) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < BLOCK_WATCH_MAX; i++) {
            if (!g_block_watches[i].enabled) { slot = i; break; }
        }
    }
    if (slot < 0) {
        fprintf(stderr, "[cpu_trace] block_watch table FULL (max=%d)\n", BLOCK_WATCH_MAX);
        fflush(stderr);
        return;
    }
    BlockWatch *w = &g_block_watches[slot];
    memset(w, 0, sizeof(*w));
    w->enabled = 1;
    w->pc24 = pc24;
    w->n_addrs = n_addrs;
    for (int i = 0; i < BLOCK_WATCH_ADDRS_MAX; i++) {
        w->ram_offsets[i] = (i < n_addrs) ? ram_offsets[i] : -1;
    }
    w->max_hits = max_hits;
    w->hit_count = 0;
    g_block_watch_any = 1;
    fprintf(stderr,
        "[cpu_trace] block_watch ARMED slot=%d pc=%06X n_addrs=%d max_hits=%d\n",
        slot, pc24, n_addrs, max_hits);
    for (int i = 0; i < n_addrs; i++) {
        fprintf(stderr, "  addr[%d] = ram_off $%05X\n", i, (uint32_t)ram_offsets[i]);
    }
    fflush(stderr);
}

void cpu_trace_block_watch_clear_all(void) {
    memset(g_block_watches, 0, sizeof(g_block_watches));
    g_block_watch_any = 0;
}

void cpu_trace_block_watch_clear_one(int slot) {
    if (slot < 0 || slot >= BLOCK_WATCH_MAX) return;
    memset(&g_block_watches[slot], 0, sizeof(BlockWatch));
    int any = 0;
    for (int i = 0; i < BLOCK_WATCH_MAX; i++) {
        if (g_block_watches[i].enabled) { any = 1; break; }
    }
    g_block_watch_any = any ? 1 : 0;
}

void cpu_trace_block_watch_check(CpuState *cpu, uint32_t pc24) {
    if (!g_block_watch_any) return;
    extern int snes_frame_counter;
    for (int i = 0; i < BLOCK_WATCH_MAX; i++) {
        BlockWatch *w = &g_block_watches[i];
        if (!w->enabled || w->pc24 != pc24) continue;
        if (w->hit_count >= w->max_hits) continue;
        BlockWatchHit *h = &w->hits[w->hit_count];
        h->frame = snes_frame_counter;
        h->A = cpu->A; h->X = cpu->X; h->Y = cpu->Y;
        h->S = cpu->S; h->D = cpu->D;
        h->DB = cpu->DB; h->PB = cpu->PB; h->P = cpu->P;
        h->m_flag = cpu->m_flag; h->x_flag = cpu->x_flag; h->e_flag = cpu->emulation;
        for (int j = 0; j < w->n_addrs; j++) {
            int32_t off = w->ram_offsets[j];
            h->vals[j] = (off >= 0 && off < 0x20000) ? g_ram[off] : 0;
        }
        int depth = g_recomp_stack_top;
        if (depth > BLOCK_WATCH_STACK_DEPTH) depth = BLOCK_WATCH_STACK_DEPTH;
        h->stack_depth = depth;
        for (int j = 0; j < depth; j++) {
            const char *fn = g_recomp_stack[g_recomp_stack_top - depth + j];
            if (fn) {
                strncpy(h->stack[j], fn, BLOCK_WATCH_FUNC_LEN - 1);
                h->stack[j][BLOCK_WATCH_FUNC_LEN - 1] = 0;
            } else {
                h->stack[j][0] = 0;
            }
        }
        w->hit_count++;
        return;  /* one slot per pc24 */
    }
}

void cpu_trace_block(CpuState *cpu, uint32_t pc24) {
    /* Investigation: block-boundary DB shadow. Catches EVERY DB change
     * (inline PLBs bypass cpu_trace_db_change), reporting the block where it
     * was first observed + the immediately-preceding block (which did it).
     * Same window as SNESRECOMP_DBTRACE. */
    {
        extern int snes_frame_counter;
        static int dbs_init = 0, dbs_lo = -1, dbs_hi = -1;
        static uint8_t dbs_last = 0xFF;
        static uint32_t dbs_prev_pc = 0;
        if (!dbs_init) {
            dbs_init = 1;
            const char *_e = getenv("SNESRECOMP_DBTRACE");
            if (_e) sscanf(_e, "%d-%d", &dbs_lo, &dbs_hi);
        }
        if (dbs_lo >= 0 && cpu->DB != dbs_last &&
            snes_frame_counter >= dbs_lo && snes_frame_counter <= dbs_hi) {
            fprintf(stderr, "[dbs] f=%d DB $%02X->$%02X in block $%06X "
                    "(prev block $%06X) S=%04X\n",
                    snes_frame_counter, dbs_last, cpu->DB, dbs_prev_pc, pc24,
                    cpu->S);
        }
        dbs_last = cpu->DB;
        dbs_prev_pc = pc24;
    }
    /* Inspection-freeze trigger (reusable). Armed once from the
     * environment so it covers the very first frames with no
     * arming race; the TCP command can also set g_freeze_at_frame. */
    {
        static int s_freeze_env_read = 0;
        if (!s_freeze_env_read) {
            s_freeze_env_read = 1;
            const char *e = getenv("SNESRECOMP_FREEZE_AT_FRAME");
            if (e && *e) g_freeze_at_frame = atoi(e);
        }
    }
    if (g_freeze_at_frame > 0 && !g_freeze_capture) {
        extern int snes_frame_counter;
        if (snes_frame_counter >= g_freeze_at_frame) {
            g_freeze_capture = 1;
            g_boundary_frozen = 1;
            fprintf(stderr,
                    "[freeze_at_frame] froze all rings at frame %d "
                    "(target %d) trace_idx=%llu boundary_idx=%llu\n",
                    snes_frame_counter, g_freeze_at_frame,
                    (unsigned long long)g_cpu_trace_idx,
                    (unsigned long long)g_boundary_idx);
            fflush(stderr);
        }
    }
    if (g_freeze_capture) return;
    capture(cpu, pc24, CPU_TR_BLOCK, 0, 0);
#if SNESRECOMP_TRACE
    dbg_oam_block_trace(cpu, pc24);  /* task #7 PC-range block path trace */
#endif
    phantom_check(cpu, pc24);
    cpu_trace_block_watch_check(cpu, pc24);
    debug_server_on_trace_block(cpu, pc24);
    cpu_trace_mx_async_check(cpu, pc24);
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
    /* DB tripwire poll — gen code does `cpu->DB = cpu_read8(...)` at PLB
     * sites directly (bypassing cpu_trace_db_change). Poll at block
     * granularity so a sane→target transition that happened since the
     * last block fires the tripwire. We carry a per-process "last seen
     * DB" so the trip fires on transition rather than steady-state.
     * pc24 here is the BLOCK pc, an upper bound on the mutation site. */
    if (g_db_tripwire.armed && !g_db_tripwire.triggered) {
        static uint8_t s_last_seen_db = 0;
        static uint8_t s_last_init = 0;
        if (!s_last_init) { s_last_seen_db = cpu->DB; s_last_init = 1; }
        if (cpu->DB != s_last_seen_db) {
            cpu_trace_db_tripwire_check(cpu, pc24, s_last_seen_db, cpu->DB,
                                        CPU_TR_BLOCK);
            s_last_seen_db = cpu->DB;
        }
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
    if (g_freeze_capture) return;  /* deliberate inspection-freeze */
    /* Investigation (env-gated): log DB/PB at every function entry inside a
     * frame window so a data-bank divergence can be located by chain.
     * SNESRECOMP_DBTRACE="lo-hi". Off by default, zero cost when unset. */
    {
        extern int snes_frame_counter;
        static int dbt_init = 0, dbt_lo = -1, dbt_hi = -1;
        if (!dbt_init) {
            dbt_init = 1;
            const char *_e = getenv("SNESRECOMP_DBTRACE");
            if (_e) sscanf(_e, "%d-%d", &dbt_lo, &dbt_hi);
        }
        if (dbt_lo >= 0 && snes_frame_counter >= dbt_lo &&
            snes_frame_counter <= dbt_hi)
            fprintf(stderr, "[dbt] f=%d DB=%02X PB=%02X S=%04X %s\n",
                    snes_frame_counter, cpu->DB, cpu->PB, cpu->S,
                    name ? name : "?");
    }
    /* Soundness check (one-shot, gated by armed flag). Compares the
     * decoder's static (M, X) claim — encoded in the function name's
     * trailing `_M{m}X{x}` suffix — against the runtime cpu->m_flag /
     * x_flag. Mismatch trips with a full snapshot and stops further
     * checks. See docs/ABSTRACT_INTERPRETATION_GAPS.md. */
    cpu_trace_mx_claim_check(cpu, pc24, name);

    int slot = (int)(g_cpu_trace_idx++ & (g_cpu_trace_capacity - 1));
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
        /* Capture full stack snapshot so probes can answer "what called
         * this function?" without ring reconstruction. Defensive: works
         * even if g_recomp_stack overflowed. */
        FuncWatchHit *fwh = &g_func_watch_hit;
        memset(fwh, 0, sizeof(*fwh));
        fwh->captured = 1;
        extern int snes_frame_counter;
        fwh->frame = snes_frame_counter;
        fwh->pc24 = pc24;
        if (cpu) {
            fwh->A = cpu->A; fwh->X = cpu->X; fwh->Y = cpu->Y;
            fwh->S = cpu->S; fwh->D = cpu->D;
            fwh->DB = cpu->DB; fwh->PB = cpu->PB; fwh->P = cpu->P;
            fwh->m_flag = cpu->m_flag; fwh->x_flag = cpu->x_flag;
            fwh->e_flag = cpu->emulation;
        }
        if (g_func_watch_name) {
            strncpy(fwh->name, g_func_watch_name, sizeof(fwh->name) - 1);
        }
        int depth = g_recomp_stack_top;
        if (depth > 64) depth = 64;
        fwh->stack_depth = depth;
        int skip = g_recomp_stack_top - depth;
        for (int i = 0; i < depth; i++) {
            const char *p = g_recomp_stack[skip + i];
            if (p) {
                strncpy(fwh->stack[i], p, sizeof(fwh->stack[i]) - 1);
            }
        }
        cpu_trace_dump_dbpb(tag);
        cpu_trace_dump_recent(tag, 128);
    }
}

void cpu_trace_event(CpuState *cpu, uint32_t pc24, uint8_t event_type,
                     uint8_t extra0, uint16_t extra1) {
    capture(cpu, pc24, event_type, extra0, extra1);
}

/* ── Phantom-PC trap ──────────────────────────────────────────────────
 * Tracks a small registered set of PCs (e.g. SMC-phantom JSR (abs,X)
 * sites) and captures a snapshot the first time any of them hits as a
 * real cpu_trace_block PC. Subsequent hits at the same PC just bump
 * .repeat_count. Cost on the hot path is one linear scan over the
 * armed set (≤32 entries) per cpu_trace_block call.
 */

PhantomTrapHit g_phantom_trap_hits[PHANTOM_TRAP_MAX] = {0};
int g_phantom_trap_hit_count = 0;
int g_phantom_trap_armed_count = 0;

/* Registered PCs (parallel array; .pc24 = 0 = unused slot). */
static struct {
    uint8_t  used;
    uint32_t pc24;
    char     label[48];
} s_phantom_arm[PHANTOM_TRAP_MAX] = {0};

void cpu_trace_phantom_disarm_all(void) {
    for (int i = 0; i < PHANTOM_TRAP_MAX; i++) {
        s_phantom_arm[i].used = 0;
        s_phantom_arm[i].pc24 = 0;
        s_phantom_arm[i].label[0] = '\0';
    }
    for (int i = 0; i < PHANTOM_TRAP_MAX; i++) {
        memset(&g_phantom_trap_hits[i], 0, sizeof(g_phantom_trap_hits[i]));
    }
    g_phantom_trap_hit_count = 0;
    g_phantom_trap_armed_count = 0;
}

void cpu_trace_phantom_arm(uint32_t pc24, const char *label) {
    for (int i = 0; i < PHANTOM_TRAP_MAX; i++) {
        if (s_phantom_arm[i].used && s_phantom_arm[i].pc24 == pc24) {
            return;  /* already armed */
        }
    }
    for (int i = 0; i < PHANTOM_TRAP_MAX; i++) {
        if (!s_phantom_arm[i].used) {
            s_phantom_arm[i].used = 1;
            s_phantom_arm[i].pc24 = pc24;
            if (label) {
                strncpy(s_phantom_arm[i].label, label,
                        sizeof(s_phantom_arm[i].label) - 1);
                s_phantom_arm[i].label[sizeof(s_phantom_arm[i].label) - 1] = '\0';
            } else {
                s_phantom_arm[i].label[0] = '\0';
            }
            g_phantom_trap_armed_count++;
            return;
        }
    }
    fprintf(stderr,
            "[cpu_trace] phantom_arm: registry full, dropped PC $%06X\n",
            pc24);
}

/* Walk backward through the cpu_trace ring for the last N CPU_TR_BLOCK
 * events and copy their PCs into `out[0..out_max]` oldest-first. */
static int phantom_collect_block_history(uint32_t *out, int out_max) {
    if (!g_cpu_trace_ring || g_cpu_trace_capacity == 0) return 0;
    /* Walk back from idx-1 (most recent) collecting BLOCK events until
     * we have out_max or run out of ring capacity. */
    int collected = 0;
    uint32_t tmp[64];
    int tmp_max = (out_max > 64) ? 64 : out_max;
    uint64_t idx = g_cpu_trace_idx;
    uint64_t cap = g_cpu_trace_capacity;
    uint64_t walked = 0;
    while (collected < tmp_max && walked < cap && idx > 0) {
        idx--;
        walked++;
        CpuTraceEvent *e = &g_cpu_trace_ring[idx & (cap - 1)];
        if (e->event_type == CPU_TR_BLOCK) {
            tmp[collected++] = e->pc24;
        }
    }
    /* Reverse so out is oldest-first. */
    for (int i = 0; i < collected; i++) {
        out[i] = tmp[collected - 1 - i];
    }
    return collected;
}

static void phantom_capture_hit(int armed_idx, CpuState *cpu, uint32_t pc24) {
    /* Find or allocate a hit slot. Same-PC hits reuse the same slot. */
    int hit_idx = -1;
    for (int i = 0; i < g_phantom_trap_hit_count; i++) {
        if (g_phantom_trap_hits[i].captured &&
            g_phantom_trap_hits[i].pc24 == pc24) {
            g_phantom_trap_hits[i].repeat_count++;
            return;
        }
    }
    if (g_phantom_trap_hit_count < PHANTOM_TRAP_MAX) {
        hit_idx = g_phantom_trap_hit_count++;
    } else {
        return;
    }
    PhantomTrapHit *h = &g_phantom_trap_hits[hit_idx];
    memset(h, 0, sizeof(*h));
    h->captured = 1;
    h->pc24 = pc24;
    if (s_phantom_arm[armed_idx].label[0]) {
        strncpy(h->label, s_phantom_arm[armed_idx].label,
                sizeof(h->label) - 1);
    }
    extern int snes_frame_counter;
    h->first_frame = snes_frame_counter;
    h->first_block_idx = g_cpu_trace_idx;
    h->repeat_count = 1;
    if (cpu) {
        h->A = cpu->A; h->X = cpu->X; h->Y = cpu->Y;
        h->S = cpu->S; h->D = cpu->D;
        h->DB = cpu->DB; h->PB = cpu->PB; h->P = cpu->P;
        h->m_flag = cpu->m_flag; h->x_flag = cpu->x_flag;
        h->e_flag = cpu->emulation;
    }
    int depth = g_recomp_stack_top;
    if (depth > 16) depth = 16;
    int skip = g_recomp_stack_top - depth;
    h->stack_depth = depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) {
            strncpy(h->stack[i], p, sizeof(h->stack[i]) - 1);
        }
    }
    h->block_history_depth = phantom_collect_block_history(h->block_history, 64);
    /* Loud diagnostic — an armed phantom PC fired, which means the
     * static SMC-phantom classification was wrong for this site OR a
     * real indirect dispatch is happening here. Caller should treat
     * this as a hard sign to stop and inspect. */
    fprintf(stderr,
            "[PHANTOM-TRAP HIT] PC=$%06X label='%s' frame=%d "
            "S=$%04X PB=$%02X DB=$%02X m=%d x=%d  "
            "stack_top=%s\n",
            pc24, h->label, h->first_frame, h->S, h->PB, h->DB,
            h->m_flag, h->x_flag,
            depth > 0 ? h->stack[depth - 1] : "<empty>");
}

/* Hot-path check called from cpu_trace_block. Linear scan over the
 * armed set; tiny (≤32) so no special data structure needed. */
static inline void phantom_check(CpuState *cpu, uint32_t pc24) {
    if (g_phantom_trap_armed_count == 0) return;
    for (int i = 0; i < PHANTOM_TRAP_MAX; i++) {
        if (s_phantom_arm[i].used && s_phantom_arm[i].pc24 == pc24) {
            phantom_capture_hit(i, cpu, pc24);
            return;
        }
    }
}

void cpu_trace_phantom_arm_smc_set(void) {
    /* The 11 unique CALL_INDIRECT sites cf_debt_report flagged on
     * 2026-05-03. The classification expects all 11 to NEVER fire as
     * real instruction PCs at runtime. If any do, this trap will
     * capture a snapshot for inspection. */
    cpu_trace_phantom_arm(0x009AE6, "HandleMenuCursor_$801D");
    cpu_trace_phantom_arm(0x00D07E, "CheckPowerUp_$601D");
    cpu_trace_phantom_arm(0x00D64C, "HandlePlayerPhysics_$A41D");
    cpu_trace_phantom_arm(0x00EF93, "RunPlayerBlockCode_$9C1D");
    cpu_trace_phantom_arm(0x00EFB9, "RunPlayerBlockCode_$601D");
    cpu_trace_phantom_arm(0x00FD9E, "SpawnGlitter_$0410");
    cpu_trace_phantom_arm(0x00FDD0, "SpawnWaterSplash_$A517");
    cpu_trace_phantom_arm(0x00FEB8, "SpawnFireball_$A91D");
    cpu_trace_phantom_arm(0x01FE63, "bank01_FE60_$00F4");
    cpu_trace_phantom_arm(0x028015, "DropReservedItem_$A21D");
    cpu_trace_phantom_arm(0x05B350, "GiveCoins_$AD1D");
    fprintf(stderr,
            "[cpu_trace] phantom-PC trap armed for %d SMC-phantom sites\n",
            g_phantom_trap_armed_count);
}

/* ── Unresolved-cross-fn-goto trap ───────────────────────────────────
 *
 * Emitted in-line by codegen at every site where decoder couldn't
 * resolve a goto target. Keys on (gen function name + source pc24 +
 * target label) so sibling variants sharing source PCs are
 * disambiguated — unlike the coarser block-PC phantom trap, this
 * fires only when the SPECIFIC C body containing the unresolved goto
 * actually executes.
 */
UnresolvedGotoHit g_unresolved_goto_hits[UNRESOLVED_GOTO_TRAP_MAX] = {0};
int g_unresolved_goto_hit_count = 0;

RecompReturn cpu_trace_unresolved_goto_trap(
    CpuState *cpu, uint32_t source_pc24, uint32_t target_pc24,
    const char *func_name, const char *target_label)
{
    /* Find or allocate a slot keyed by (func_name, source_pc24). Same
     * gen function calling the same goto-source site reuses the slot;
     * different variants (M0X1 vs M1X1 of the same asm function) get
     * SEPARATE slots because their func_name differs. */
    int hit_idx = -1;
    for (int i = 0; i < g_unresolved_goto_hit_count; i++) {
        UnresolvedGotoHit *h = &g_unresolved_goto_hits[i];
        if (!h->captured) continue;
        if (h->source_pc24 == source_pc24
            && strncmp(h->func_name, func_name ? func_name : "",
                       sizeof(h->func_name)) == 0)
        {
            h->repeat_count++;
            return RECOMP_RETURN_NORMAL;
        }
    }
    if (g_unresolved_goto_hit_count < UNRESOLVED_GOTO_TRAP_MAX) {
        hit_idx = g_unresolved_goto_hit_count++;
    } else {
        /* Slots exhausted — still loud-log so we know this happened. */
        fprintf(stderr,
                "[UNRESOLVED-GOTO TRAP - slots full] source=$%06X "
                "target=$%06X func='%s' label='%s'\n",
                source_pc24, target_pc24,
                func_name ? func_name : "?",
                target_label ? target_label : "?");
        return RECOMP_RETURN_NORMAL;
    }
    UnresolvedGotoHit *h = &g_unresolved_goto_hits[hit_idx];
    memset(h, 0, sizeof(*h));
    h->captured = 1;
    h->source_pc24 = source_pc24;
    h->target_pc24 = target_pc24;
    h->repeat_count = 1;
    if (func_name) {
        strncpy(h->func_name, func_name, sizeof(h->func_name) - 1);
    }
    if (target_label) {
        strncpy(h->target_label, target_label, sizeof(h->target_label) - 1);
    }
    extern int snes_frame_counter;
    h->first_frame = snes_frame_counter;
    h->first_block_idx = g_cpu_trace_idx;
    if (cpu) {
        h->A = cpu->A; h->X = cpu->X; h->Y = cpu->Y;
        h->S = cpu->S; h->D = cpu->D;
        h->DB = cpu->DB; h->PB = cpu->PB; h->P = cpu->P;
        h->m_flag = cpu->m_flag; h->x_flag = cpu->x_flag;
        h->e_flag = cpu->emulation;
    }
    int depth = g_recomp_stack_top;
    if (depth > 16) depth = 16;
    int skip = g_recomp_stack_top - depth;
    h->stack_depth = depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) strncpy(h->stack[i], p, sizeof(h->stack[i]) - 1);
    }
    h->block_history_depth = phantom_collect_block_history(h->block_history, 64);
    /* Loud diagnostic — codegen used to silently return here. */
    fprintf(stderr,
            "[UNRESOLVED-GOTO TRAP HIT] func='%s' source=$%06X "
            "target=$%06X label='%s' frame=%d S=$%04X PB=$%02X DB=$%02X "
            "m=%d x=%d  stack_top=%s\n",
            h->func_name, source_pc24, target_pc24,
            h->target_label, h->first_frame, h->S, h->PB, h->DB,
            h->m_flag, h->x_flag,
            depth > 0 ? h->stack[depth - 1] : "<empty>");
    return RECOMP_RETURN_NORMAL;
}

/* ── Unresolved-stub trap ────────────────────────────────────────────
 *
 * Stub bodies in src/gen/unresolved_stubs_v2.c chain into this. A
 * fire means a generated function actually called a target whose
 * bank wasn't in the cfg set — i.e. either real code reaches data
 * decoded as code (recompiler bug) or a phantom variant got executed.
 */
UnresolvedStubHit g_unresolved_stub_hits[UNRESOLVED_STUB_TRAP_MAX] = {0};
int g_unresolved_stub_hit_count = 0;

RecompReturn cpu_trace_unresolved_stub_trap(
    CpuState *cpu, uint32_t target_pc24, const char *func_name)
{
    int hit_idx = -1;
    for (int i = 0; i < g_unresolved_stub_hit_count; i++) {
        UnresolvedStubHit *h = &g_unresolved_stub_hits[i];
        if (!h->captured) continue;
        if (h->target_pc24 == target_pc24
            && strncmp(h->func_name, func_name ? func_name : "",
                       sizeof(h->func_name)) == 0)
        {
            h->repeat_count++;
            return RECOMP_RETURN_NORMAL;
        }
    }
    if (g_unresolved_stub_hit_count < UNRESOLVED_STUB_TRAP_MAX) {
        hit_idx = g_unresolved_stub_hit_count++;
    } else {
        fprintf(stderr,
                "[UNRESOLVED-STUB TRAP - slots full] target=$%06X func='%s'\n",
                target_pc24, func_name ? func_name : "?");
        return RECOMP_RETURN_NORMAL;
    }
    UnresolvedStubHit *h = &g_unresolved_stub_hits[hit_idx];
    memset(h, 0, sizeof(*h));
    h->captured = 1;
    h->target_pc24 = target_pc24;
    h->repeat_count = 1;
    if (func_name) {
        strncpy(h->func_name, func_name, sizeof(h->func_name) - 1);
    }
    extern int snes_frame_counter;
    h->first_frame = snes_frame_counter;
    h->first_block_idx = g_cpu_trace_idx;
    if (cpu) {
        h->A = cpu->A; h->X = cpu->X; h->Y = cpu->Y;
        h->S = cpu->S; h->D = cpu->D;
        h->DB = cpu->DB; h->PB = cpu->PB; h->P = cpu->P;
        h->m_flag = cpu->m_flag; h->x_flag = cpu->x_flag;
        h->e_flag = cpu->emulation;
    }
    int depth = g_recomp_stack_top;
    if (depth > 16) depth = 16;
    int skip = g_recomp_stack_top - depth;
    h->stack_depth = depth;
    for (int i = 0; i < depth; i++) {
        const char *p = g_recomp_stack[skip + i];
        if (p) strncpy(h->stack[i], p, sizeof(h->stack[i]) - 1);
    }
    h->block_history_depth = phantom_collect_block_history(h->block_history, 64);
    fprintf(stderr,
            "[UNRESOLVED-STUB TRAP HIT] func='%s' target=$%06X "
            "frame=%d S=$%04X PB=$%02X DB=$%02X m=%d x=%d stack_top=%s\n",
            h->func_name, target_pc24, h->first_frame, h->S,
            h->PB, h->DB, h->m_flag, h->x_flag,
            depth > 0 ? h->stack[depth - 1] : "<empty>");
    return RECOMP_RETURN_NORMAL;
}

/* ── Indirect-dispatch OOB trap ──────────────────────────────────────
 *
 * Generated code calls this when a runtime index register exceeds the
 * cfg-declared `count` at an `indirect_dispatch` site. Reuses the
 * UnresolvedStubHit slot table so existing inspection commands
 * (`unresolved_stub_get`) surface the hit without adding a parallel
 * data structure. `func_name` is synthesised from the site PC.
 *
 * Returns RECOMP_RETURN_NORMAL so the caller (the dispatcher's switch
 * default arm) returns cleanly — the misindexed call is suppressed
 * but the runtime doesn't crash. A hit is ALWAYS a real bug to chase
 * (cfg count too small, runtime register corruption, etc.); the
 * stderr line tags it loudly.
 */
RecompReturn cpu_trace_dispatch_oob(
    CpuState *cpu, uint32_t site_pc24, uint32_t value)
{
    char name[64];
    snprintf(name, sizeof(name), "dispatch_oob_$%06X[%06X]",
             (unsigned)(site_pc24 & 0xFFFFFFu),
             (unsigned)(value & 0xFFFFFFu));
    /* Delegate to the unresolved-stub path for slot accounting +
     * stderr emission. target_pc24 carries the full miss value; the
     * synthesized name carries the dispatch site. */
    return cpu_trace_unresolved_stub_trap(cpu, value & 0xFFFFFFu, name);
}

void cpu_trace_phantom_arm_unresolvable_goto_set(void) {
    /* Unresolvable-cross-fn-goto block-entry PCs cf_debt_report
     * 2026-05-03 found. Each is the BLOCK PC whose codegen currently
     * emits `return RECOMP_RETURN_NORMAL; /\* ... unresolvable cross-fn
     * goto ... *\/` — a silent normalisation of unknown control
     * transfer. The trap captures whether any of these PCs actually
     * runs; if 0 hits across attract + gameplay, flipping the emit
     * to a loud abort is safe. If hits, the silent-return is
     * masking a real bug. */
    cpu_trace_phantom_arm(0x009D38, "BufferFileSelectText_unres");
    cpu_trace_phantom_arm(0x00EE2D, "RunPlayerBlockCode_00EE1D_unres");
    cpu_trace_phantom_arm(0x018B06, "SprXXX_Generic_018B03_unres");
    cpu_trace_phantom_arm(0x038839, "Spr0BF_MegaMole_038820_unres");
    cpu_trace_phantom_arm(0x03FE00, "bank_03_FE00_unres");
    cpu_trace_phantom_arm(0x059C60, "auto_059C60_unres");
    cpu_trace_phantom_arm(0x05C32B, "UnusedScrollSprite_unres");
    cpu_trace_phantom_arm(0x0DB583, "ExtObj8E_YellowSwitchBlock_unres");
    cpu_trace_phantom_arm(0x0DB916, "RopeObj33_BlueSwitchBlocks_unres");
    fprintf(stderr,
            "[cpu_trace] phantom-PC trap now covers %d total sites "
            "(SMC + unresolvable-goto)\n",
            g_phantom_trap_armed_count);
}

/* Default-on so frame-822 stack ops are captured from boot without TCP
 * arming latency. cmd_stack_op_disable can clear when not needed. */
uint8_t g_stack_op_trace_enabled = 1;

void cpu_trace_stack_op(CpuState *cpu, uint32_t pc24, uint8_t op_id,
                        uint16_t old_S, int8_t delta) {
    if (!g_stack_op_trace_enabled) return;
    /* extra0 = op_id, extra1 = (uint16)((uint8)delta << 8) — sign-extended at
     * read time. Old S goes in addr16; new S in new_value (these are unused
     * for non-WRAM events, free to repurpose). */
    capture(cpu, pc24, CPU_TR_STACK_OP, op_id,
            (uint16_t)((uint16_t)(uint8_t)delta << 8));
    uint64_t just_idx = g_cpu_trace_idx - 1;
    CpuTraceEvent *just = &g_cpu_trace_ring[just_idx & (g_cpu_trace_capacity - 1)];
    just->addr16 = old_S;
    just->new_value = cpu->S;
    just->old_value = old_S;
    just->width = 0;
    just->bank = 0;
}

static int db_watch_hit(uint8_t db) {
    return (g_db_watch_bits[db >> 5] >> (db & 0x1F)) & 1u;
}

void cpu_trace_db_change(CpuState *cpu, uint32_t pc24, uint8_t old_db,
                         uint8_t new_db, uint8_t event_type) {
    /* Investigation: windowed DB-WRITE log (same window as SNESRECOMP_DBTRACE).
     * Every DB change with its PC so the unfaithful write can be checked
     * against the ROM ASM (no oracle needed). */
    {
        extern int snes_frame_counter;
        static int dbw_init = 0, dbw_lo = -1, dbw_hi = -1;
        if (!dbw_init) {
            dbw_init = 1;
            const char *_e = getenv("SNESRECOMP_DBTRACE");
            if (_e) sscanf(_e, "%d-%d", &dbw_lo, &dbw_hi);
        }
        if (dbw_lo >= 0 && new_db != old_db &&
            snes_frame_counter >= dbw_lo && snes_frame_counter <= dbw_hi)
            fprintf(stderr, "[dbw] f=%d PC=$%06X DB $%02X->$%02X S=%04X\n",
                    snes_frame_counter, pc24, old_db, new_db, cpu->S);
    }
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
        /* One-shot: disarm before dumping so a corruption loop cycling DB
         * through the watched value can't restream the ring 60×/sec. */
        cpu_trace_set_db_watch(new_db, 0);
        cpu_trace_dump_dbpb(tag);
        cpu_trace_dump_recent(tag, 256);
    }
    /* One-shot DB tripwire — captures structured snapshot for TCP query
     * on first transition to target_db. Cheap when not armed (single
     * load+branch on g_db_tripwire.armed). */
    cpu_trace_db_tripwire_check(cpu, pc24, old_db, new_db, event_type);
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
/* FuncWatchHit struct definition is in cpu_trace.h so debug_server.c
 * can read fields directly via the extern declaration. */
FuncWatchHit g_func_watch_hit = {0};
void cpu_trace_set_func_watch(const char *name) {
    extern uint32_t fnv1a_extern(const char *s);
    g_func_watch_name = name;
    g_func_watch_hash = name ? fnv1a_extern(name) : 0;
    /* Reset captured state so the next hit takes a fresh snapshot. */
    memset(&g_func_watch_hit, 0, sizeof(g_func_watch_hit));
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
                                int32_t ram_off, uint16_t old_val,
                                uint16_t new_val, int width) {
    if (g_freeze_capture) return;  /* deliberate inspection-freeze */
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
        /* Always record. extra0 = new value, extra1 = (bank<<8) | hit_byte
         * (legacy back-compat). New B2 fields land directly on the
         * captured event after capture() returns: explicit
         * bank/width/addr16/old_value/new_value eliminate the
         * "arm one byte at a time" workaround. */
        uint32_t pc24 = ((uint32_t)cpu->PB << 16); /* low 16 unknown at write site */
        capture(cpu, pc24, CPU_TR_WRAM_WRITE, hit_val,
                (uint16_t)(((uint16_t)bank << 8) | (uint16_t)hit_byte));
        /* The just-captured event is at index (g_cpu_trace_idx - 1). */
        {
            uint64_t just_idx = g_cpu_trace_idx - 1;
            CpuTraceEvent *just = &g_cpu_trace_ring[just_idx & (g_cpu_trace_capacity - 1)];
            just->bank = bank;
            just->width = (uint8_t)width;
            just->addr16 = (uint16_t)(addr + (uint16_t)hit_byte);
            just->old_value = old_val;
            just->new_value = new_val;
        }
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

/* Off-rails detection: catch impossible CPU paths (RomPtr-invalid,
 * cart-out-of-range, etc.) and record per-bucket context for later
 * inspection via TCP. Buckets dedupe by (tag, high 16 bits of hint)
 * so a sweep through e.g. $BB:0000-$BB:FFFF collapses to one bucket.
 *
 * 2026-05-16: stderr output reduced from 1024 DB/PB lines + 64
 * trace lines per bucket (caused multi-second mid-gameplay stalls
 * while stderr drained) to a single one-line summary. Full context
 * (first/last hint, frame, recomp stack top) is captured per bucket
 * and queryable via the `offrails_get` TCP command. */
/* `struct OffRailsBucket` is declared in cpu_trace.h so the TCP layer
 * can read its fields. The string-field sizes (24 and 64) MUST match
 * the literal sizes in the header struct decl. */
#define OFFRAILS_TAG_MAX  32
#define OFFRAILS_TAG_LEN  24
#define OFFRAILS_FUNC_LEN 64
static OffRailsBucket s_offrails_seen[OFFRAILS_TAG_MAX];
static int s_offrails_used = 0;

/* Accessors for the TCP layer in debug_server.c. */
int cpu_trace_offrails_count(void) { return s_offrails_used; }
const OffRailsBucket *cpu_trace_offrails_bucket(int i) {
    if (i < 0 || i >= s_offrails_used) return NULL;
    return &s_offrails_seen[i];
}

/* ── NLR diagnostic counters (non-rotating) ────────────────────────── */
NlrDiag g_nlr_diag = {0};

static int nlr_diag_find_or_add_site(const char *name) {
    uint32_t h = name ? fnv1a(name) : 0;
    for (int i = 0; i < g_nlr_diag.per_site_used; i++) {
        if (g_nlr_diag.per_site_hash[i] == h) return i;
    }
    if (g_nlr_diag.per_site_used >= NLR_DIAG_PER_SITE_MAX) return -1;
    int slot = g_nlr_diag.per_site_used++;
    g_nlr_diag.per_site_hash[slot] = h;
    g_nlr_diag.per_site_count[slot] = 0;
    if (name) {
        strncpy(g_nlr_diag.per_site_label[slot], name, NLR_DIAG_FIRST_FUNC_LEN - 1);
        g_nlr_diag.per_site_label[slot][NLR_DIAG_FIRST_FUNC_LEN - 1] = 0;
    } else {
        g_nlr_diag.per_site_label[slot][0] = 0;
    }
    return slot;
}

void cpu_trace_nlr_site_exec(CpuState *cpu, uint32_t pc24, const char *name) {
    (void)cpu; (void)pc24;
    g_nlr_diag.site_exec_count++;
    int slot = nlr_diag_find_or_add_site(name);
    if (slot >= 0) g_nlr_diag.per_site_count[slot]++;
}

void cpu_trace_pending_skip_write(CpuState *cpu, uint32_t pc24,
                                  uint8_t new_value, const char *func) {
    (void)cpu;
    g_nlr_diag.pending_skip_writes++;
    if (!g_nlr_diag.first_writer_captured && new_value != 0) {
        extern int snes_frame_counter;
        g_nlr_diag.first_writer_captured = 1;
        g_nlr_diag.first_writer_pc24 = pc24;
        g_nlr_diag.first_writer_frame = snes_frame_counter;
        g_nlr_diag.first_writer_value = new_value;
        if (func) {
            strncpy(g_nlr_diag.first_writer_func, func, NLR_DIAG_FIRST_FUNC_LEN - 1);
            g_nlr_diag.first_writer_func[NLR_DIAG_FIRST_FUNC_LEN - 1] = 0;
        }
    }
}

void cpu_trace_pending_skip_consume(CpuState *cpu, uint32_t pc24,
                                    uint8_t value, const char *func) {
    (void)cpu;
    if (value == 0) {
        g_nlr_diag.pending_skip_reads_zero++;
    } else {
        g_nlr_diag.pending_skip_reads_nonzero++;
        if (!g_nlr_diag.first_consumer_captured) {
            extern int snes_frame_counter;
            g_nlr_diag.first_consumer_captured = 1;
            g_nlr_diag.first_consumer_pc24 = pc24;
            g_nlr_diag.first_consumer_frame = snes_frame_counter;
            g_nlr_diag.first_consumer_value = value;
            if (func) {
                strncpy(g_nlr_diag.first_consumer_func, func,
                        NLR_DIAG_FIRST_FUNC_LEN - 1);
                g_nlr_diag.first_consumer_func[NLR_DIAG_FIRST_FUNC_LEN - 1] = 0;
            }
        }
    }
}

void cpu_trace_offrails(const char *tag, uint32_t hint) {
    /* Group by (tag, high 16 bits of hint) so a sweep through e.g.
     * $BB:0000-$BB:FFFF dedupes to one bucket. */
    uint64_t group_k = fnv1a(tag ? tag : "");
    group_k = (group_k * 0x100000001B3ull) ^ ((uint64_t)hint & 0xFFFF0000u);
    extern int snes_frame_counter;
    for (int i = 0; i < s_offrails_used; i++) {
        if (s_offrails_seen[i].hash == group_k) {
            s_offrails_seen[i].hit_count++;
            s_offrails_seen[i].last_frame = snes_frame_counter;
            s_offrails_seen[i].last_hint = hint;
            return;  /* silent dedup */
        }
    }
    if (s_offrails_used >= OFFRAILS_TAG_MAX) return;  /* table full, silent */
    OffRailsBucket *b = &s_offrails_seen[s_offrails_used++];
    memset(b, 0, sizeof(*b));
    b->hash        = group_k;
    b->hit_count   = 1;
    b->first_frame = snes_frame_counter;
    b->last_frame  = snes_frame_counter;
    b->first_hint  = hint;
    b->last_hint   = hint;
    if (tag) {
        strncpy(b->tag, tag, OFFRAILS_TAG_LEN - 1);
        b->tag[OFFRAILS_TAG_LEN - 1] = 0;
    }
    if (g_recomp_stack_top > 0 && g_recomp_stack[g_recomp_stack_top - 1]) {
        strncpy(b->stack_top, g_recomp_stack[g_recomp_stack_top - 1],
                OFFRAILS_FUNC_LEN - 1);
        b->stack_top[OFFRAILS_FUNC_LEN - 1] = 0;
    }
    /* ONE-LINE stderr summary. Full ring-buffer context queryable via
     * the TCP `offrails_get` command — no multi-second stall from a
     * 1088-line stderr flood at the moment of the hit. */
    fprintf(stderr,
            "[off-rails] [%s] hit=$%06X frame=%d stack_top=%s "
            "(group=$%02X:_ — query offrails_get for context)\n",
            tag ? tag : "?", hint & 0xFFFFFF, snes_frame_counter,
            b->stack_top[0] ? b->stack_top : "<empty>",
            (unsigned)((hint >> 16) & 0xFF));
}

/* Public wrapper so debug_server / other TUs can hash names. */
uint32_t fnv1a_extern(const char *s) { return fnv1a(s); }

void cpu_trace_arm_default_watches(void) {
    /* DB-poison auto-watch — SMW-specific heuristic ("DB outside
     * $00-$0D / $7E-$7F is poisoning"). For MMX and other games that
     * legitimately use high DB banks, every novel bank transition
     * dump-streams 256 trace events to stderr and stalls the runner.
     * Opt in via SNESRECOMP_DB_POISON_WATCH=1; arm specific banks at
     * runtime via the TCP `db_watch_add` command otherwise. */
    {
        const char *v = getenv("SNESRECOMP_DB_POISON_WATCH");
        if (v && v[0] && v[0] != '0') {
            for (int b = 0x80; b <= 0xFF; b++)
                cpu_trace_set_db_watch((uint8_t)b, 1);
            for (int b = 0x10; b <= 0x7D; b++) {
                if (b == 0x00 || b == 0x01 || b == 0x02 || b == 0x03 ||
                    b == 0x04 || b == 0x05 || b == 0x07 || b == 0x0C ||
                    b == 0x0D) continue;
                cpu_trace_set_db_watch((uint8_t)b, 1);
            }
        }
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
            "PB!=0, S out-of-$01XX-$1FFF, "
            "GameMode14_InLevel_0086DF, WRAM recorders on $7E:008A/8B/8C "
            "(DB-poison auto-watch off; SNESRECOMP_DB_POISON_WATCH=1 to enable)\n");
    /* Arm P.X tripwire at startup so the first 1→0 transition is caught
     * even before TCP attaches. The snapshot doesn't rotate. */
    cpu_trace_arm_px_tripwire();
    fprintf(stderr, "[cpu_trace] P.X tripwire armed (first 1→0 transition caught)\n");
    /* Auto-arm DB→$C0 tripwire — investigation 2026-05-02: cpu->DB
     * corrupts to $C0 at every ProcessGameMode entry (DBPB ring shows
     * the steady-state but not the first transition). This catches the
     * FIRST sane→$C0 transition with full boundary-event history. */
    {
        uint8_t trip_target = 0xC0;
        const char *v = getenv("SNESRECOMP_DB_TRIP_TARGET");
        if (v && v[0]) {
            char *endp = NULL;
            unsigned long parsed = strtoul(v, &endp, 16);
            if (endp != v) trip_target = (uint8_t)(parsed & 0xFFu);
        }
        cpu_trace_arm_db_tripwire(trip_target);
        fprintf(stderr,
                "[cpu_trace] DB tripwire armed (first transition to $%02X)\n",
                trip_target);
    }
    /* Auto-arm SMC-phantom PC trap — investigation 2026-05-03: the 11
     * unique CALL_INDIRECT sites cf_debt_report flagged are decoder
     * phantoms (M=0 wrong-mode decode of M=1 SMC-dispatch byte
     * sequences). This trap catches any of those PCs actually firing
     * as block entries — proves the classification empirically. */
    cpu_trace_phantom_arm_smc_set();
    /* Auto-arm unresolvable-cross-fn-goto trap — investigation
     * 2026-05-03: 9 unique block PCs whose codegen currently emits
     * `return RECOMP_RETURN_NORMAL` for a goto whose target lies
     * outside cfg's import range. Trap proves whether the silent
     * normalisation is actually exercised before we change emit to
     * a loud abort. */
    cpu_trace_phantom_arm_unresolvable_goto_set();
    /* Stack-drift tripwire is NOT auto-armed. The boundary ring is
     * already always-on and queryable via `boundary_get tail N` to
     * find any function whose exit_S != entry_S. The tripwire's only
     * value was an automated stderr printout + boundary-history
     * struct snapshot, both of which were a net loss because firing
     * it also froze the live cpu_trace + boundary rings (see
     * capture() and boundary_audit_record_*) — destroying observation
     * of every event after the trip. The handler `bank_00_84C3`
     * (legitimate IRQ-exit: PLB/PLD/PLY/PLX/PLA + RTI) is a guaranteed
     * +9 false positive against any naive S-balance check, and the
     * MMX attract path has multiple other intentional S-imbalanced
     * RTI handlers. Arm explicitly via TCP `stack_drift_arm` if a
     * specific hunt wants the snapshot — but prefer ring queries. */
    /* Auto-arm static M/X claim verifier — catches decoder-soundness
     * gaps at function entry, BEFORE stack drift cascades into visible
     * symptoms. Trips on first variant-suffix mismatch. See
     * docs/ABSTRACT_INTERPRETATION_GAPS.md for the model. */
    cpu_trace_arm_mx_claim_check();
    fprintf(stderr, "[cpu_trace] static M/X claim verifier armed\n");
    /* Auto-arm async m_flag/x_flag write tripwire — catches the class
     * of bug where something asynchronous (NMI/IRQ state restoration
     * is the prime suspect, per ISSUES.md DA49 entry) flips
     * cpu->m_flag or cpu->x_flag without going through an emitted
     * SEP/REP/PHP/PLP/RTI/XCE block. */
    cpu_trace_arm_mx_async_check();
    fprintf(stderr, "[cpu_trace] async M/X flag-write tripwire armed\n");
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
    /* Auto-arm the MMIO register-write trace at boot so the FIRST
     * DMA-related setups (which fire around frame ~94 — too early
     * for TCP attach + manual `trace_reg`) end up in the always-on
     * 32K ring. Plus arm a targeted DMA tripwire that fires on the
     * specific bad upload pattern (VRAM dst in $7000-$8FFF + DMA
     * source bank $05). 2026-04-30. */
    extern void debug_server_arm_default_reg_trace(void);
    extern void debug_server_arm_default_dma_tripwire(void);
    extern void debug_server_arm_default_wram_trace(void);
    debug_server_arm_default_reg_trace();
    debug_server_arm_default_dma_tripwire();
    debug_server_arm_default_wram_trace();
    fprintf(stderr, "[cpu_trace] reg_trace + dma_tripwire + wram_trace armed at boot\n");
    /* Per-byte recorder on the surrounding region $7E:1925-$1930 so the
     * WRM ring captures the COMPLETE timeline of writes around the
     * corruption site. Walk backward from the trip to see which
     * preceding bytes ($192E, $192F) were written legitimately and
     * which were collateral from the bad 16-bit STA. */
    for (int a = 0x1925; a <= 0x1930; a++) {
        cpu_trace_set_wram_watch(0x7E, (uint16_t)a, 1, 0, 0, 1);
    }
    /* MMX cooperative-scheduler task-slot table: 7 slots × 16-byte
     * stride starting at $7E:0030. Each slot's handler PC sits at
     * offset $32-$33 (low-byte, high-byte). Investigation 2026-05-22:
     * slot 0's $33 flips from $85 (Task0 $852C) to $00 around
     * frame 8834, after which the scheduler tears down Task0's fiber
     * and tries to dispatch invalid $002C. Per-byte recorder on every
     * slot's handler-PC pair captures whichever store(s) overwrite
     * $33 (and by symmetry the high byte of any other slot's PC), so
     * any future scheduler-state corruption walks backward from the
     * trip into the always-on ring. */
    for (int slot = 0; slot < 7; slot++) {
        uint16_t hi = (uint16_t)(0x0033 + slot * 0x10);
        uint16_t lo = (uint16_t)(0x0032 + slot * 0x10);
        cpu_trace_set_wram_watch(0x7E, lo, 1, 0, 0, 1);
        cpu_trace_set_wram_watch(0x7E, hi, 1, 0, 0, 1);
    }
    /* MMX DMA queue tail pointers. Two distinct queue uploaders exist
     * in the NMI chain:
     *   $7E:00A3 — fixed-stride-8 queue at $DB:0500. Walker is
     *     bank_00_82C8's $00:8336-$8371 loop. `BCS $836C` trap fires
     *     if the X-by-8 advance overflows past the value at $A3.
     *   $7E:00A5 — variable-size queue at $7E:F000. Walker is the
     *     $00:BA0A-$BA4D loop (reached via JSR from $82C8 / nearby).
     *     Each entry is 4-byte header + size-byte data; loop ends
     *     when X equals the word at $A5. `BCS $BA48` trap fires if
     *     the ADC #$size advance overflows past $800. 2026-05-23: this
     *     trap is the actual Dr. Light capsule freeze site — queue
     *     contained two valid 4+$40 entries (total $88 bytes) but
     *     $A5 was $80, so the walker overshot, read garbage entries,
     *     and overflowed.
     * Per-byte recorders cover both queue tail pointers and the
     * $A6 high byte (for 16-bit STA at $A5). */
    for (int a = 0x00A2; a <= 0x00A6; a++) {
        cpu_trace_set_wram_watch(0x7E, (uint16_t)a, 1, 0, 0, 1);
    }
    fflush(stderr);
}

void cpu_trace_clear(void) {
    if (g_cpu_trace_ring && g_cpu_trace_capacity)
        memset(g_cpu_trace_ring, 0,
               (size_t)g_cpu_trace_capacity * sizeof(CpuTraceEvent));
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
    if ((uint64_t)n > g_cpu_trace_capacity) n = (int)g_cpu_trace_capacity;
    if ((uint64_t)n > g_cpu_trace_idx) n = (int)g_cpu_trace_idx;
    /* Single-buffer dump (see cpu_trace_dump_dbpb for the rationale).
     * Up to ~160 bytes per line — a full event line with the longest
     * decoration variant is ~140 chars. Bound the buffer at 64 KB so
     * even a worst-case n=256 dump fits comfortably. Caller passes
     * n <= 256 in practice (DB-WATCH path passes 256). */
    static char buf[64 * 1024];
    int cap = (int)sizeof(buf);
    int pos = snprintf(buf, cap,
                       "=== %s — last %d trace events ===\n"
                       "  (newest first)\n",
                       tag ? tag : "trace", n);
    for (int i = 0; i < n && pos < cap - 192; i++) {
        uint64_t abs_idx = g_cpu_trace_idx - 1 - i;
        int slot = (int)(abs_idx & (g_cpu_trace_capacity - 1));
        CpuTraceEvent *e = &g_cpu_trace_ring[slot];
        pos += snprintf(buf + pos, cap - pos,
                        "  [%-5s] PC=$%06X DB=%02X PB=%02X A=%04X X=%04X Y=%04X S=%04X "
                        "P=%02X m=%u x=%u",
                        event_name(e->event_type), e->pc24, e->DB, e->PB,
                        e->A, e->X, e->Y, e->S, e->P, e->M, e->XF);
        switch (e->event_type) {
            case CPU_TR_PLB:
            case CPU_TR_PHB:
            case CPU_TR_DB_WRITE:
                pos += snprintf(buf + pos, cap - pos,
                                "  DB %02X→%02X", e->extra0, (uint8_t)e->extra1);
                break;
            case CPU_TR_PHK:
            case CPU_TR_PB_WRITE:
            case CPU_TR_JSL:
            case CPU_TR_RTL:
                pos += snprintf(buf + pos, cap - pos,
                                "  PB %02X→%02X", e->extra0, (uint8_t)e->extra1);
                break;
            case CPU_TR_MVN:
            case CPU_TR_MVP:
                pos += snprintf(buf + pos, cap - pos,
                                "  src=%02X dst=%02X", e->extra0, (uint8_t)e->extra1);
                break;
            case CPU_TR_FUNC_ENTRY:
                pos += snprintf(buf + pos, cap - pos,
                                "  hash=%08X", e->native_func_id_or_hash);
                break;
            case CPU_TR_WRAM_WRITE:
                pos += snprintf(buf + pos, cap - pos,
                                "  bank=%02X byte=%u newval=$%02X",
                                (uint8_t)(e->extra1 >> 8),
                                (uint8_t)(e->extra1 & 0xFF), e->extra0);
                break;
        }
        pos += snprintf(buf + pos, cap - pos, "\n");
    }
    fwrite(buf, 1, (size_t)pos, stderr);
    fflush(stderr);
}

void cpu_trace_dump_wram(const char *tag, int scan_n) {
    int total = (int)((g_cpu_trace_idx < g_cpu_trace_capacity) ?
                      g_cpu_trace_idx : g_cpu_trace_capacity);
    if (scan_n <= 0 || scan_n > total) scan_n = total;
    fprintf(stderr, "=== %s — WRAM-WRITE events in last %d ring entries (newest first) ===\n",
            tag ? tag : "wram", scan_n);
    /* Walk backward and remember the most-recent BLOCK / FUNC for each
     * WRAM_WRITE we emit, so the caller can attribute each write to a
     * function context without dumping the entire ring. */
    int wram_count = 0;
    for (int i = 0; i < scan_n && wram_count < 256; i++) {
        uint64_t abs_idx = g_cpu_trace_idx - 1 - (uint64_t)i;
        int slot = (int)(abs_idx & (g_cpu_trace_capacity - 1));
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
            int prev_slot = (int)(prev_abs & (g_cpu_trace_capacity - 1));
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
            int prev_slot = (int)(prev_abs & (g_cpu_trace_capacity - 1));
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
    /* Build the whole dump in a single stack-local buffer and emit
     * with one fwrite. Per-line fprintf to stderr on Windows console
     * is a WriteConsole syscall + console formatting per call; 1024
     * lines blocks the CPU thread for ~1 s of wall time. With one
     * fwrite the same dump takes microseconds and the thread keeps
     * running. ~80 bytes/line is plenty for the fixed layout below. */
    static char buf[CPU_DBPB_RING_LEN * 80 + 256];
    int cap = (int)sizeof(buf);
    int pos = snprintf(buf, cap,
                       "=== %s — last %d DB/PB mutations ===\n",
                       tag ? tag : "dbpb", n);
    for (int i = 0; i < n && pos < cap - 96; i++) {
        uint64_t abs_idx = g_cpu_dbpb_idx - 1 - i;
        int slot = (int)(abs_idx & (CPU_DBPB_RING_LEN - 1));
        CpuDbpbEvent *d = &g_cpu_dbpb_ring[slot];
        pos += snprintf(buf + pos, cap - pos,
                        "  [%-5s] PC=$%06X %s %02X→%02X (S=$%04X)\n",
                        event_name(d->event_type), d->pc24,
                        d->reg_id == 0 ? "DB" : "PB",
                        d->old_val, d->new_val, d->S);
    }
    fwrite(buf, 1, (size_t)pos, stderr);
    fflush(stderr);
}

#endif /* SNESRECOMP_TRACE */
