#pragma once

/* cpu_trace.h — Backwards watcher for v2 SMW boot debugging.
 *
 * Two ring buffers + per-event hooks, all gated on SNESRECOMP_TRACE.
 * Compile-out cleanly when the macro is unset (every helper is a no-op
 * inline so Release|x64 ships the same as before).
 *
 * Goal: when cpu->DB or cpu->PB get poisoned, a `dump recent` from the
 * crash handler (or via debug-server cmd) tells us the EXACT prior
 * instructions and the FIRST mutation that produced the bad state.
 * Stack-deep crash output ("we died in foo") is necessary but not
 * sufficient — we need backwards visibility.
 *
 * Two rings:
 *   1. CpuTraceEvent[CPU_TRACE_RING_LEN]: every basic-block entry +
 *      every targeted state-mutation event. PCs + register snapshot.
 *   2. CpuDbpbEvent[CPU_DBPB_RING_LEN]: smaller ring of ONLY DB/PB
 *      mutations (PHK, PLB, PHB, PHK, PLP, MVN/MVP, RTL/JSL bank
 *      transitions). Survives churn in the main ring; lets us answer
 *      "show me the last 16 bank changes."
 *
 * Tripwires:
 *   cpu_trace_set_db_watch(byte): if cpu->DB gets set to that value,
 *      dump the rings to stderr immediately (caller-driven; we don't
 *      poll inside hot paths).
 */

#include "types.h"
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

/* Event-type IDs for the targeted hooks. */
enum {
    CPU_TR_BLOCK    = 0,   /* basic-block entry */
    CPU_TR_PHB      = 1,
    CPU_TR_PLB      = 2,
    CPU_TR_PHK      = 3,
    CPU_TR_PLP      = 4,
    CPU_TR_PHP      = 5,
    CPU_TR_RTI      = 6,
    CPU_TR_JSL      = 7,
    CPU_TR_RTL      = 8,
    CPU_TR_MVN      = 9,
    CPU_TR_MVP      = 10,
    CPU_TR_DB_WRITE = 11,  /* any direct cpu->DB mutation */
    CPU_TR_PB_WRITE = 12,  /* any direct cpu->PB mutation */
    CPU_TR_FUNC_ENTRY = 13,  /* generated function entry */
    CPU_TR_WRAM_WRITE = 14,  /* watched WRAM byte/word write */
};

typedef struct CpuTraceEvent {
    uint32_t pc24;                   /* SNES PC at event time (bank<<16 | local) */
    uint32_t native_func_id_or_hash; /* fnv-1a of function name, optional */
    uint16_t A;
    uint16_t X;
    uint16_t Y;
    uint16_t S;
    uint16_t D;
    uint8_t  DB;
    uint8_t  PB;
    uint8_t  P;
    uint8_t  M;
    uint8_t  XF;
    uint8_t  event_type;             /* one of CPU_TR_* */
    uint8_t  extra0;                 /* event-specific (e.g. old DB) */
    uint16_t extra1;                 /* event-specific (e.g. old PB | new) */
} CpuTraceEvent;

typedef struct CpuDbpbEvent {
    uint32_t pc24;
    uint8_t  event_type;
    uint8_t  reg_id;     /* 0 = DB, 1 = PB */
    uint8_t  old_val;
    uint8_t  new_val;
    uint16_t S;          /* stack at the time, useful for PLB */
    uint16_t pad;
} CpuDbpbEvent;

/* Ring sizes: with always-on continuous capture there's no reason to
 * keep these tight. 1M main events @ ~32B/entry = ~32MB; fine on
 * modern hosts and big enough to hold the entire boot phase plus
 * many seconds of game state for backwards investigation, even at
 * high block rates. Keep DB/PB ring smaller because mutations are
 * rare. */
#define CPU_TRACE_RING_LEN  (1024 * 1024)
#define CPU_DBPB_RING_LEN   1024

#if SNESRECOMP_TRACE

extern CpuTraceEvent g_cpu_trace_ring[CPU_TRACE_RING_LEN];
extern uint64_t      g_cpu_trace_idx;     /* monotonic; modulo with LEN */
extern CpuDbpbEvent  g_cpu_dbpb_ring[CPU_DBPB_RING_LEN];
extern uint64_t      g_cpu_dbpb_idx;
extern uint8_t       g_db_watch_set;       /* bitmask: bit N set => watch DB == N (256 bits packed in 32B) */
extern uint32_t      g_db_watch_bits[8];

void cpu_trace_block(CpuState *cpu, uint32_t pc24);
void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name);
void cpu_trace_event(CpuState *cpu, uint32_t pc24, uint8_t event_type,
                     uint8_t extra0, uint16_t extra1);

/* Specialised helpers — record the PRE/POST values of DB/PB mutations and
 * mirror them into the small DB/PB ring. PC24 is the source-line PC of
 * the instruction performing the mutation. Calls cpu_trace_event() for
 * the main ring AND records into the dbpb ring. Tripwire fires inside
 * if the new DB matches a watched value. */
void cpu_trace_db_change(CpuState *cpu, uint32_t pc24, uint8_t old_db,
                         uint8_t new_db, uint8_t event_type);
void cpu_trace_pb_change(CpuState *cpu, uint32_t pc24, uint8_t old_pb,
                         uint8_t new_pb, uint8_t event_type);

void cpu_trace_set_db_watch(uint8_t db_byte, int enabled);
void cpu_trace_set_pb_watch(uint8_t pb_byte, int enabled);
void cpu_trace_set_s_range_watch(uint16_t s_lo, uint16_t s_hi, int enabled);

/* WRAM-address watch.
 *
 * Fires when a watched WRAM byte/word is written through cpu_write8 /
 * cpu_write16. Up to CPU_WRAM_WATCH_MAX simultaneous watches are
 * supported; if `match_value` is non-zero, the watch only fires when
 * the new low byte equals `value` (so you can ask "tell me when $7E:008c
 * becomes $57" instead of "tell me every write to $7E:008c").
 *
 * The watch hooks the cpu_write* path, so it captures gen-code stores
 * but NOT direct g_ram[off] writes from hand-body C. Hand bodies that
 * matter should route through CPU_WRAM_WRITE_TRACE() (see common_rtl).
 *
 * The bank field is matched modulo SNES WRAM mirroring: a watch at
 * ($7E, $008c) also fires for writes to ($00, $008c) etc. — they hit the
 * same g_ram offset. */
#define CPU_WRAM_WATCH_MAX 32

typedef struct WramWatch {
    uint8_t  enabled;
    uint8_t  match_value;   /* 0 = any, 1 = only when new_val == value */
    uint8_t  value;
    uint8_t  width;         /* 1 or 2 (informational; check happens per-byte) */
    int32_t  ram_offset;    /* g_ram offset; -1 = not WRAM */
    uint8_t  bank;          /* original bank for dump display */
    uint16_t addr;          /* original addr for dump display */
} WramWatch;

extern WramWatch g_wram_watches[CPU_WRAM_WATCH_MAX];

void cpu_trace_set_wram_watch(uint8_t bank, uint16_t addr, int width,
                              int match_value, uint8_t value, int enabled);
void cpu_trace_clear_wram_watches(void);
/* Called from cpu_write8 / cpu_write16 AFTER the store completes. The
 * RAM offset has already been computed by the caller; pass it through
 * so we don't recompute the bank/addr → offset map here. `width` is 1
 * or 2 (the call site decides). For 16-bit writes the helper checks
 * each watched offset against off and off+1 separately so a watch on
 * the high byte still fires when STZ touches both bytes. */
void cpu_trace_wram_write_check(CpuState *cpu, uint8_t bank, uint16_t addr,
                                int32_t ram_off, uint16_t new_val, int width);
/* If `name` matches a function entry, fire a one-shot trace dump and
 * disarm. Useful for "did the empty fallback stub get called?" probes
 * (e.g. arm on "GameMode14_InLevel_0086DF" to catch the next miss). */
void cpu_trace_set_func_watch(const char *name);

/* Arm the standard SMW v2-boot watch set: high-bank DBs ($A0-$FF), any
 * PB != 0, S outside $0100-$1FFF, and known fallback stub names.
 * Called at process startup so the watcher is USEFUL by default —
 * without this, tripwires sit idle until manually armed. */
void cpu_trace_arm_default_watches(void);

/* Called by RomPtr-invalid + cart_readLorom-out-of-range + any other
 * "off-the-rails" softfail to dump the trace ONCE per N events. Avoids
 * burying the trace under repeats of the same fail. */
void cpu_trace_offrails(const char *tag, uint32_t hint);
void cpu_trace_clear(void);

/* ── Scoped one-shot tripwire (TCP-readable) ──────────────────────────────
 *
 * Arm with a (bank, addr_lo, addr_hi) WRAM range and an optional substring
 * the recomp stack must contain. On the FIRST cpu_write* hit that matches
 * BOTH criteria, capture a snapshot and disarm. The snapshot is intended
 * for TCP query — debug_server reads g_scoped_tripwire and emits JSON
 * including the triggering event, recomp stack at trip time, DP region
 * snapshot, and the absolute trace ring index so the client can query
 * surrounding events.
 *
 * Distinct from `cpu_trace_set_wram_watch(..., match_value=1, value=V)`
 * which fires a STDERR dump on a value-match. This tripwire fires on
 * any write inside the address range while a named function is on the
 * stack, captures structured data, and stays captured until the client
 * reads or rearms.
 */
#define SCOPED_TRIPWIRE_STACK_DEPTH 16
#define SCOPED_TRIPWIRE_DP_BYTES    32     /* $7E:0080-$009F snapshot */
#define SCOPED_TRIPWIRE_GM_BYTES    16     /* $7E:0100-$010F snapshot */
#define SCOPED_TRIPWIRE_FUNC_LEN    48
#define SCOPED_TRIPWIRE_CONTEXT_FN  48     /* most-recent func name @ trip */

typedef struct ScopedTripwire {
    /* Arming state */
    uint8_t  armed;
    uint8_t  triggered;
    uint8_t  bank;              /* "armed" bank — used for JSON readback only */
    uint8_t  width_seen;        /* 1 or 2 — width of the triggering write */
    uint16_t addr_lo;
    uint16_t addr_hi;
    /* Canonical ram_off range: any write whose ram_off lands in this
     * inclusive range fires the tripwire, regardless of which mirror
     * bank the gen code wrote through. SMW commonly writes to low DP
     * via DB=$00 ($00:0100), which `cpu_ram_offset` maps to the same
     * g_ram offset as $7E:0100. Bank-strict matching missed those. */
    int32_t  ram_off_lo;
    int32_t  ram_off_hi;
    char     scope_substr[SCOPED_TRIPWIRE_FUNC_LEN];
    /* Optional value-match: when match_enabled=1, the trip only fires
     * when the byte that lands at the watched offset equals match_val.
     * Catches both 8-bit STA (low byte) and 16-bit STA at addr-1 (high
     * byte) — the cpu_write* check loop handles both cases via
     * hit_byte_in_word. Use case 2026-04-30: catch the rare write of
     * $FA into $7E:1930 (corruption) without the tripwire firing on
     * the dozens of legitimate prior writes of other values. */
    uint8_t  match_enabled;
    uint8_t  match_val;

    /* Captured at trip time */
    int      frame;
    uint64_t main_cycles;
    uint64_t trace_idx;          /* absolute g_cpu_trace_idx at trip */
    uint64_t block_counter;

    uint16_t hit_addr;           /* the byte offset that matched */
    uint8_t  hit_val;
    uint8_t  hit_byte_in_word;   /* 0=low, 1=high — for 16-bit writes */

    /* Full CpuState at trip */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;

    /* Most-recent context */
    uint32_t recent_block_pc24;  /* last cpu_trace_block PC before trip */
    uint32_t recent_func_pc24;   /* last cpu_trace_func_entry PC before trip */
    char     last_func_name[SCOPED_TRIPWIRE_CONTEXT_FN];

    /* Recomp stack snapshot (deepest on top, [0] is bottom-of-stack) */
    int      stack_depth;
    char     stack[SCOPED_TRIPWIRE_STACK_DEPTH][SCOPED_TRIPWIRE_FUNC_LEN];

    /* DP/GM region snapshots */
    uint8_t  dp_snapshot[SCOPED_TRIPWIRE_DP_BYTES];   /* $7E:0080-009F */
    uint8_t  gm_snapshot[SCOPED_TRIPWIRE_GM_BYTES];   /* $7E:0100-010F */
    /* DP-low snapshot $7E:0000-001F. Many palette/stripe loops use
     * DP $00-$02 as the 24-bit source pointer; at trip we want the
     * actual pointer value to compare against the oracle. */
    uint8_t  dp_low_snapshot[32];                       /* $7E:0000-001F */
} ScopedTripwire;

extern ScopedTripwire g_scoped_tripwire;

/* Arm the tripwire. Pass scope_substr=NULL for "any stack". */
void cpu_trace_arm_scoped_tripwire(uint8_t bank, uint16_t addr_lo,
                                   uint16_t addr_hi, const char *scope_substr);
/* Arm the tripwire with a value-match. Trip only fires when the byte
 * landing at the watched offset == match_val. Catches both 8-bit STA
 * and the high byte of an overlapping 16-bit STA. */
void cpu_trace_arm_scoped_tripwire_v(uint8_t bank, uint16_t addr_lo,
                                     uint16_t addr_hi, const char *scope_substr,
                                     uint8_t match_val);
void cpu_trace_disarm_scoped_tripwire(void);

/* ── P.X tripwire (non-rotating snapshot) ─────────────────────────────────
 *
 * Distinct from the WRAM ScopedTripwire above. Arms BEFORE the rotating
 * trace ring evicts boot-time events. Fires the FIRST time P.X (status
 * register bit 4) transitions from 1 → 0. Captures a snapshot that
 * survives ring rotation: full CpuState, recomp stack, last-N P-mutation
 * events copied OUT of the rotating ring into a frozen ring inside the
 * snapshot.
 *
 * Hooked into:
 *   - cpu_trace_p_change(): every gen-emitted REP/SEP/PLP/RTI calls this
 *     to log packed-P mutations.
 *   - cpu_trace_p_sync_check(): runtime helper sync helpers (P-from-mirrors)
 *     call this to verify nothing else clears X without going through gen.
 *
 * The breadcrumb ring lives INSIDE the tripwire snapshot itself, so even
 * if the main g_cpu_trace_ring rotates a million times, the captured
 * P-mutation history at trip-time is preserved.
 */
#define PX_TRIPWIRE_PMUT_RING  64
#define PX_TRIPWIRE_STACK_DEPTH 16
#define PX_TRIPWIRE_FUNC_LEN   48

typedef struct PxPMutEvent {
    uint32_t pc24;            /* PC of the instruction causing the mutation */
    uint8_t  source_kind;     /* 0=REP, 1=SEP, 2=PLP, 3=RTI, 4=PHP, 5=p_to_mirrors, 6=mirrors_to_p, 7=other */
    uint8_t  old_p;
    uint8_t  new_p;
    uint8_t  old_x_flag;
    uint8_t  new_x_flag;
    uint16_t S;
    uint16_t pad;
} PxPMutEvent;

typedef struct PxBreadcrumb {
    uint32_t marker;          /* user-supplied tag id */
    uint8_t  P;
    uint8_t  m_flag;
    uint8_t  x_flag;
    uint8_t  e_flag;
    uint16_t A;
    uint16_t X;
    uint16_t Y;
    uint16_t S;
    uint16_t D;
    uint8_t  DB;
    uint8_t  PB;
    uint8_t  pad[2];
    char     label[PX_TRIPWIRE_FUNC_LEN];
} PxBreadcrumb;

#define PX_BREADCRUMB_MAX 32

typedef struct PxTripwire {
    uint8_t  armed;
    uint8_t  triggered;
    uint8_t  pad[2];

    /* P-mutation ring inside the snapshot. Filled continuously while
     * armed (regardless of trip); on trip, contents are FROZEN by setting
     * triggered=1 (further mutations stop being recorded). */
    PxPMutEvent pmut_ring[PX_TRIPWIRE_PMUT_RING];
    uint32_t    pmut_write_idx;     /* monotonic; modulo PX_TRIPWIRE_PMUT_RING */
    uint32_t    pmut_count;         /* min(write_idx, RING_LEN) */

    /* Reset-tail breadcrumbs — explicit checkpoints user code adds via
     * cpu_trace_px_breadcrumb(marker, "label"). Linear, no rotation. */
    PxBreadcrumb breadcrumbs[PX_BREADCRUMB_MAX];
    uint32_t     breadcrumb_count;

    /* Captured at trip time */
    PxPMutEvent  trip_event;        /* the mutation that caused the trip */
    uint32_t     trip_trace_idx;    /* g_cpu_trace_idx at trip */

    /* Full CpuState at trip */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;

    /* Recomp stack snapshot */
    int      stack_depth;
    char     stack[PX_TRIPWIRE_STACK_DEPTH][PX_TRIPWIRE_FUNC_LEN];
    char     last_func[PX_TRIPWIRE_FUNC_LEN];
} PxTripwire;

extern PxTripwire g_px_tripwire;

/* Arm the P.X tripwire. Fires once on first P.X 1→0 transition. */
void cpu_trace_arm_px_tripwire(void);
void cpu_trace_disarm_px_tripwire(void);
void cpu_trace_clear_px_tripwire(void);

/* Called by gen / helpers when packed P is mutated. source_kind picks the
 * tag printed in the snapshot (REP/SEP/PLP/RTI/PHP/sync/other).
 * Cheap when not armed — single load+branch on g_px_tripwire.armed. */
void cpu_trace_px_record(CpuState *cpu, uint32_t pc24, uint8_t source_kind,
                         uint8_t old_p, uint8_t new_p);

/* Coarse breadcrumb — emit a labelled checkpoint into the tripwire's
 * non-rotating breadcrumb array. Use these around suspect call sites in
 * I_RESET / smw_rtl. Cheap (memcpy of CpuState fields). */
void cpu_trace_px_breadcrumb(CpuState *cpu, uint32_t marker, const char *label);

/* Dump the last `n` events of the main ring to stderr, prefixed by `tag`. */
void cpu_trace_dump_recent(const char *tag, int n);
/* Dump the entire dbpb ring (newest first). */
void cpu_trace_dump_dbpb(const char *tag);
/* Filtered dump: walk backward over the main ring (up to `scan_n` events)
 * and print only CPU_TR_WRAM_WRITE events plus the most-recent BLOCK or
 * FUNC_ENTRY that PRECEDED each (so we know who was running when the
 * write happened). When `scan_n <= 0`, scans the entire ring.
 * Newest-first ordering. */
void cpu_trace_dump_wram(const char *tag, int scan_n);

#else  /* SNESRECOMP_TRACE = 0 */

static inline void cpu_trace_block(CpuState *cpu, uint32_t pc24)            { (void)cpu; (void)pc24; }
static inline void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name) { (void)cpu; (void)pc24; (void)name; }
static inline void cpu_trace_event(CpuState *cpu, uint32_t pc24, uint8_t et,
                                   uint8_t e0, uint16_t e1)                 { (void)cpu; (void)pc24; (void)et; (void)e0; (void)e1; }
static inline void cpu_trace_db_change(CpuState *cpu, uint32_t pc24, uint8_t o,
                                       uint8_t n, uint8_t et)               { (void)cpu; (void)pc24; (void)o; (void)n; (void)et; }
static inline void cpu_trace_pb_change(CpuState *cpu, uint32_t pc24, uint8_t o,
                                       uint8_t n, uint8_t et)               { (void)cpu; (void)pc24; (void)o; (void)n; (void)et; }
static inline void cpu_trace_set_db_watch(uint8_t b, int e)                 { (void)b; (void)e; }
static inline void cpu_trace_set_pb_watch(uint8_t b, int e)                 { (void)b; (void)e; }
static inline void cpu_trace_set_s_range_watch(uint16_t l, uint16_t h, int e){ (void)l; (void)h; (void)e; }
static inline void cpu_trace_set_wram_watch(uint8_t b, uint16_t a, int w, int mv, uint8_t v, int e) { (void)b; (void)a; (void)w; (void)mv; (void)v; (void)e; }
static inline void cpu_trace_clear_wram_watches(void) { }
static inline void cpu_trace_wram_write_check(CpuState *c, uint8_t b, uint16_t a, int32_t off, uint16_t nv, int w) { (void)c; (void)b; (void)a; (void)off; (void)nv; (void)w; }
static inline void cpu_trace_set_func_watch(const char *n)                  { (void)n; }
static inline void cpu_trace_arm_default_watches(void)                       { }
static inline void cpu_trace_offrails(const char *t, uint32_t h)            { (void)t; (void)h; }
static inline void cpu_trace_clear(void)                                    { }
static inline void cpu_trace_dump_recent(const char *tag, int n)            { (void)tag; (void)n; }
static inline void cpu_trace_dump_dbpb(const char *tag)                     { (void)tag; }
static inline void cpu_trace_dump_wram(const char *tag, int n)              { (void)tag; (void)n; }
static inline void cpu_trace_arm_scoped_tripwire(uint8_t b, uint16_t l, uint16_t h, const char *s) { (void)b; (void)l; (void)h; (void)s; }
static inline void cpu_trace_disarm_scoped_tripwire(void) { }
static inline void cpu_trace_arm_px_tripwire(void) { }
static inline void cpu_trace_disarm_px_tripwire(void) { }
static inline void cpu_trace_clear_px_tripwire(void) { }
static inline void cpu_trace_px_record(CpuState *c, uint32_t p, uint8_t k, uint8_t o, uint8_t n) { (void)c; (void)p; (void)k; (void)o; (void)n; }
static inline void cpu_trace_px_breadcrumb(CpuState *c, uint32_t m, const char *l) { (void)c; (void)m; (void)l; }

#endif

#ifdef __cplusplus
}
#endif
