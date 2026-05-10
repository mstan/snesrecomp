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
    /* Non-local return signaling (see cpu_state.h::RecompReturn).
     * extra0 carries the SKIP_N count, extra1 carries the recomp
     * stack depth at the event. */
    CPU_TR_NLR_DETECT  = 15,  /* a non-local-return idiom block fired */
    CPU_TR_NLR_PROPAGATE = 16,  /* a callsite forwarded a SKIP_N up the stack */
    CPU_TR_NLR_CONSUMED = 17,  /* a callsite received SKIP_N and decremented */
    /* Per-instruction stack-op trace. Captures every PHA/PHX/PHY/PHP/PHB/PHD/PHK,
     * PEA/PEI/PER, PLA/PLX/PLY/PLP/PLB/PLD, and RTS/RTL/RTI as they execute.
     * extra0 = stack-op mnemonic id (see CPU_STACK_OP_*).
     * extra1 = (high byte = bytes pushed/popped, signed: +N = pushed, -N = popped;
     *          low byte = unused). Use addr16 for old S, new_value for new S
     *          (reusing the WRAM_WRITE field slots; non-WRAM events leave them
     *          zero today, so the field is free). */
    CPU_TR_STACK_OP    = 18,
};

/* Stack-op mnemonic IDs. Encoded in extra0 of CPU_TR_STACK_OP events. */
enum {
    CPU_STACK_OP_PHA = 1,
    CPU_STACK_OP_PHX = 2,
    CPU_STACK_OP_PHY = 3,
    CPU_STACK_OP_PHP = 4,
    CPU_STACK_OP_PHB = 5,
    CPU_STACK_OP_PHD = 6,
    CPU_STACK_OP_PHK = 7,
    CPU_STACK_OP_PEA = 8,
    CPU_STACK_OP_PEI = 9,
    CPU_STACK_OP_PER = 10,
    CPU_STACK_OP_PLA = 11,
    CPU_STACK_OP_PLX = 12,
    CPU_STACK_OP_PLY = 13,
    CPU_STACK_OP_PLP = 14,
    CPU_STACK_OP_PLB = 15,
    CPU_STACK_OP_PLD = 16,
    CPU_STACK_OP_RTS = 17,
    CPU_STACK_OP_RTL = 18,
    CPU_STACK_OP_RTI = 19,
};

typedef struct CpuTraceEvent {
    uint32_t pc24;                   /* SNES PC at event time (bank<<16 | local) */
    int32_t  frame;                  /* snes_frame_counter at event time */
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
    /* B2 (2026-05-01): explicit named fields for WRAM_WRITE events.
     * For non-WRAM events these stay zero. Old extra0/extra1 keep
     * their existing back-compat semantics. Filled directly on the
     * captured event after capture() returns (separate from the
     * generic capture() entry path). */
    uint8_t  bank;                   /* writer's bank ($00, $7E, ...) */
    uint8_t  width;                  /* WRAM_WRITE: 1 or 2 */
    uint16_t addr16;                 /* WRAM_WRITE: original 16-bit addr */
    uint16_t old_value;              /* WRAM_WRITE: pre-store value */
    uint16_t new_value;              /* WRAM_WRITE: post-store value */
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

/* Ring sizes: with always-on continuous capture, "tight" sizing forces
 * probes to attach quickly, which is the anti-pattern this project
 * rejects. Default 16M main events (~512 MB at 32B/entry) holds
 * ~16K frames at typical attract-demo block rates (~1000 events/frame)
 * — covers ~4.5 minutes of continuous play. Override via
 * SNESRECOMP_CPU_TRACE_RING_ENTRIES env (decimal, clamped to
 * [1<<16, 1<<28]). Heap-allocated at cpu_trace_init() time so the BSS
 * doesn't blow past the Windows PE 2 GB load ceiling. The DB/PB ring
 * stays small (mutations are rare). */
#define CPU_TRACE_RING_DEFAULT_ENTRIES (16ULL * 1024ULL * 1024ULL)
#define CPU_DBPB_RING_LEN   1024

#if SNESRECOMP_TRACE

/* Heap-allocated ring; pointer + capacity are mutable. The capacity is
 * always a power of 2 (the modulo math relies on it) — alloc rounds
 * down to the nearest power-of-2 if env asks for something non-pow2. */
extern CpuTraceEvent *g_cpu_trace_ring;
extern uint64_t       g_cpu_trace_capacity;  /* always pow2; mask = cap - 1 */
extern uint64_t       g_cpu_trace_idx;       /* monotonic; modulo via mask */
/* Initialise the ring (or re-allocate at a new capacity). Called once at
 * startup from main; idempotent. Returns the chosen capacity. */
uint64_t cpu_trace_init(void);
extern CpuDbpbEvent  g_cpu_dbpb_ring[CPU_DBPB_RING_LEN];
extern uint64_t      g_cpu_dbpb_idx;
extern uint8_t       g_db_watch_set;       /* bitmask: bit N set => watch DB == N (256 bits packed in 32B) */
extern uint32_t      g_db_watch_bits[8];

void cpu_trace_block(CpuState *cpu, uint32_t pc24);
void cpu_trace_func_entry(CpuState *cpu, uint32_t pc24, const char *name);
void cpu_trace_event(CpuState *cpu, uint32_t pc24, uint8_t event_type,
                     uint8_t extra0, uint16_t extra1);

/* Per-instruction stack-op trace. Gen code (or a hand-body) calls this
 * AFTER the push/pull mutates cpu->S. We capture old_S = new_S +/- delta
 * and record into the main ring as CPU_TR_STACK_OP. Off by default —
 * enable by setting g_stack_op_trace_enabled = 1 (controllable via TCP).
 * `delta` is +N for pulls (S increased), -N for pushes (S decreased). */
extern uint8_t g_stack_op_trace_enabled;
void cpu_trace_stack_op(CpuState *cpu, uint32_t pc24, uint8_t op_id,
                        uint16_t old_S, int8_t delta);

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
 * the high byte still fires when STZ touches both bytes. The caller
 * captures `old_val` before the store and passes it through so the
 * trace event preserves the pre-write byte (B2, 2026-05-01). */
void cpu_trace_wram_write_check(CpuState *cpu, uint8_t bank, uint16_t addr,
                                int32_t ram_off, uint16_t old_val,
                                uint16_t new_val, int width);
/* If `name` matches a function entry, fire a one-shot trace dump and
 * disarm. Useful for "did the empty fallback stub get called?" probes
 * (e.g. arm on "GameMode14_InLevel_0086DF" to catch the next miss). */
void cpu_trace_set_func_watch(const char *name);

/* Snapshot captured at the FIRST entry to the func_watch'd function.
 * Includes registers and a copy of g_recomp_stack[] — answers "what
 * called this function?" without ring reconstruction. */
typedef struct FuncWatchHit {
    uint8_t  captured;
    int      frame;
    uint32_t pc24;
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    char     name[64];
    int      stack_depth;
    char     stack[64][64];
} FuncWatchHit;
extern FuncWatchHit g_func_watch_hit;

/* Arm the standard SMW v2-boot watch set: high-bank DBs ($A0-$FF), any
 * PB != 0, S outside $0100-$1FFF, and known fallback stub names.
 * Called at process startup so the watcher is USEFUL by default —
 * without this, tripwires sit idle until manually armed. */
void cpu_trace_arm_default_watches(void);

/* ── Phantom-PC trap ───────────────────────────────────────────────────────
 *
 * Records every block-entry hit at any PC in a small registered set. Used
 * to PROVE that suspected decoder phantoms (e.g. JSR (abs,X) sites whose
 * bytes only exist as M=1 STA-operand bytes) never execute as real
 * instruction starts. If a registered PC actually hits at runtime, the
 * full snapshot is captured so we can see who called and what (m,x) state
 * the path entered under.
 *
 * Always-on. Cost is one bit-test per cpu_trace_block call. Off the hot
 * path of normal execution because cpu_trace_block is already gated by
 * the trace ring's enable.
 *
 * API:
 *   cpu_trace_phantom_arm(pc24, label)  - register a single PC to watch.
 *                                         Up to PHANTOM_TRAP_MAX PCs.
 *                                         label is a short tag for the
 *                                         report (e.g. "RunPlayerBlockCode_EF93").
 *   cpu_trace_phantom_disarm_all()      - clear the registered set.
 *   g_phantom_trap_hits                  - 0..PHANTOM_TRAP_MAX captured hits;
 *                                         each one is at most one (one
 *                                         hit per PC; subsequent hits
 *                                         increment .repeat_count only).
 *   g_phantom_trap_hit_count             - how many slots are populated.
 */
#define PHANTOM_TRAP_MAX 32

typedef struct PhantomTrapHit {
    uint8_t  captured;          /* 1 if this slot has fired */
    uint32_t pc24;              /* the registered PC */
    char     label[48];
    int      first_frame;
    uint64_t first_block_idx;   /* g_block_trace_idx at first hit, if available */
    int      repeat_count;
    /* Snapshot at first hit: */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    /* Recomp stack at first hit (latest 16 frames, callee->caller order). */
    int      stack_depth;
    char     stack[16][64];
    /* Last 64 block PCs preceding the trap, oldest -> newest. Filled
     * from g_block_trace ring at trip time. */
    int      block_history_depth;
    uint32_t block_history[64];
} PhantomTrapHit;

extern PhantomTrapHit g_phantom_trap_hits[PHANTOM_TRAP_MAX];
extern int g_phantom_trap_hit_count;
extern int g_phantom_trap_armed_count;

void cpu_trace_phantom_arm(uint32_t pc24, const char *label);
void cpu_trace_phantom_disarm_all(void);

/* Arm the canonical SMC-phantom set (the 11 unique CALL_INDIRECT sites
 * cf_debt_report flagged on chore/cf-debt-report). Called from
 * cpu_trace_arm_default_watches at process start. */
void cpu_trace_phantom_arm_smc_set(void);

/* Arm the unresolvable-cross-fn-goto block-entry set. These are the
 * BLOCK PCs whose codegen currently emits `return RECOMP_RETURN_NORMAL`
 * with an "unresolvable cross-fn goto" comment — silently normalising
 * an unknown control transfer into a normal return. The trap fires
 * when any such block executes so we can see whether the silent
 * fall-through is actually exercised at runtime, before flipping the
 * emit to a loud abort.
 *
 * NOTE 2026-05-03: this trap is too COARSE — it keys on PC only and
 * fires on shared source PCs inside healthy sibling variants. Use the
 * cpu_trace_unresolved_goto_trap helper below for in-codegen traps;
 * those carry the (function, source PC, target PC) tuple so a hit
 * unambiguously identifies the gen variant that ACTUALLY executed
 * the unresolvable goto. The phantom-PC variant is kept as a
 * second-line safety net. */
void cpu_trace_phantom_arm_unresolvable_goto_set(void);

/* ── Unresolved-cross-fn-goto runtime trap ─────────────────────────────────
 *
 * Replaces the historical silent `return RECOMP_RETURN_NORMAL; /\*
 * unresolvable cross-fn goto *\/` fallback. Emitted by codegen at every
 * site where the decoder couldn't resolve a goto target inside cfg's
 * import range.
 *
 * Keying: (gen function name + source pc24 + target label) — NOT just
 * the source PC. Two gen variants of the same asm function share
 * source PCs but have different bodies; a previous coarser trap fired
 * on healthy sibling-variant block hits and gave a misleading "live"
 * reading. Always pass `func_name` (the variant-mangled gen function
 * name like "SprXXX_Generic_SpriteLockedPath_018B03_M1X1") so the
 * captured snapshot identifies WHICH C body executed the goto.
 *
 * On hit: captures full register snapshot + recomp stack + 64-deep
 * block-PC history into g_unresolved_goto_hits. Loud stderr line.
 * In debug/Oracle builds calls abort() so the failure is unmistakable;
 * in Release the trap returns RECOMP_RETURN_NORMAL so the program
 * keeps running (matches historical silent behaviour) — but the
 * captured state is still queryable via TCP.
 *
 * Slot capacity is small (per-site) — repeats at the same site bump
 * .repeat_count instead of allocating new slots.
 */
#define UNRESOLVED_GOTO_TRAP_MAX 32

typedef struct UnresolvedGotoHit {
    uint8_t  captured;
    uint32_t source_pc24;
    uint32_t target_pc24;       /* 0 if target is a label-only reference */
    char     func_name[64];
    char     target_label[48];
    int      first_frame;
    uint64_t first_block_idx;
    int      repeat_count;
    /* Snapshot at first hit: */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    int      stack_depth;
    char     stack[16][64];
    int      block_history_depth;
    uint32_t block_history[64];
} UnresolvedGotoHit;

extern UnresolvedGotoHit g_unresolved_goto_hits[UNRESOLVED_GOTO_TRAP_MAX];
extern int g_unresolved_goto_hit_count;

/* Called from generated code at every unresolved-cross-fn-goto site.
 * Returns RECOMP_RETURN_NORMAL after recording the hit, so codegen
 * can chain it as `return cpu_trace_unresolved_goto_trap(...);`
 * preserving the C ABI. In Oracle/debug builds aborts after capture.
 * `func_name` and `target_label` MUST be string literals or globally
 * stable (we strncpy the bytes). */
RecompReturn cpu_trace_unresolved_goto_trap(
    CpuState *cpu, uint32_t source_pc24, uint32_t target_pc24,
    const char *func_name, const char *target_label);

/* ── Unresolved-stub runtime trap ─────────────────────────────────────
 *
 * Replaces the historical silent stub body
 *   `(void)cpu; return RECOMP_RETURN_NORMAL;`
 * emitted into src/gen_v2/unresolved_stubs_v2.c for Call targets that
 * resolve to a ROM bank not in the cfg set. These targets are
 * typically data decoded as code (garbled JSL operands from a phantom
 * function) — but a silent normal-return hides any case where one
 * actually fires at runtime.
 *
 * Keying: stub function name (e.g. "bank_24_222F_M1X1"). One slot
 * per unique stub; repeats bump .repeat_count.
 *
 * On hit: register snapshot + recomp stack + 64-deep block-PC
 * history + loud stderr line. Returns RECOMP_RETURN_NORMAL so the
 * program keeps running (matches historical silent behaviour). */
#define UNRESOLVED_STUB_TRAP_MAX 32

typedef struct UnresolvedStubHit {
    uint8_t  captured;
    uint32_t target_pc24;       /* bank<<16 | addr — encoded from name */
    char     func_name[64];
    int      first_frame;
    uint64_t first_block_idx;
    int      repeat_count;
    /* Snapshot at first hit: */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    int      stack_depth;
    char     stack[16][64];
    int      block_history_depth;
    uint32_t block_history[64];
} UnresolvedStubHit;

extern UnresolvedStubHit g_unresolved_stub_hits[UNRESOLVED_STUB_TRAP_MAX];
extern int g_unresolved_stub_hit_count;

/* Called from generated stub bodies in unresolved_stubs_v2.c. Returns
 * RECOMP_RETURN_NORMAL after recording the hit so caller continues.
 * `func_name` MUST be a string literal or globally stable (strncpy'd). */
RecompReturn cpu_trace_unresolved_stub_trap(
    CpuState *cpu, uint32_t target_pc24, const char *func_name);

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

/* ── Boundary auditor (per-function entry/exit ring) ─────────────────────
 *
 * Distinct from g_cpu_trace_ring (which is a single linear stream of
 * mixed BLOCK / FUNC_ENTRY / WRAM_WRITE / DB_WRITE / ... events). The
 * boundary ring captures EVERY generated-function entry and exit with
 * full register state at the boundary. Pairing is kept by `entry_seq`
 * on EXIT events pointing back to the originating ENTRY's `seq`.
 *
 * Hooked into RecompStackPush (entry) and RecompStackPop (exit) in
 * common_cpu_infra.c, so it fires automatically for every generated
 * function — no codegen change needed.
 *
 * Use case (2026-05-02): static push/pop audit found 131 imbalanced
 * functions but path-insensitive. Dynamic boundary auditor + DB→$C0
 * tripwire pinpoint the FIRST runtime corruption on the attract path,
 * which the static list cannot.
 */

#define BOUNDARY_NAME_LEN 40

enum {
    BD_ENTRY = 0,
    BD_EXIT  = 1,
};

/* Exit kinds — set on BD_EXIT events so post-hoc analysis can ignore
 * SKIP-cascade imbalances (which are intentional under the NLR ABI).
 * NORMAL = the function returned normally (RTS read pending_skip == 0).
 * NLR_PRIMARY = an NLR-pattern block in this function set pending_skip
 *               and the RTS returned the SKIP_N value. The function's
 *               S balance is preserved (literal PLAs were skipped).
 * SKIP_PROPAGATION = a JSR/JSL callsite in this function received
 *                    SKIP_N from its callee and emitted an early
 *                    return (without running its own post-call
 *                    cleanup). The function's S balance is
 *                    LEGITIMATELY broken — its prologue PHB/PHK
 *                    weren't matched by epilogue PLB. This mirrors
 *                    the asm "skip caller" semantic. */
enum {
    BD_EXIT_KIND_NORMAL = 0,
    BD_EXIT_KIND_NLR_PRIMARY = 1,
    BD_EXIT_KIND_SKIP_PROPAGATION = 2,
};

typedef struct BoundaryEvent {
    uint64_t seq;            /* monotonic event id (matches g_boundary_idx) */
    uint64_t entry_seq;      /* for BD_EXIT: seq of paired BD_ENTRY; 0 for BD_ENTRY */
    int32_t  frame;          /* snes_frame_counter at boundary */
    uint32_t name_hash;      /* fnv-1a of name */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P;
    uint8_t  m_flag, x_flag;
    uint8_t  kind;           /* BD_ENTRY | BD_EXIT */
    uint8_t  stack_depth;    /* g_recomp_stack_top AFTER push (entry) or BEFORE pop (exit) */
    uint8_t  exit_kind;      /* one of BD_EXIT_KIND_*; only meaningful when kind == BD_EXIT */
    uint8_t  pad[2];
    char     name[BOUNDARY_NAME_LEN];
} BoundaryEvent;

/* Default ring: 1M events × 80B = ~80 MB. Holds ~250-1000 frames at
 * typical attract-demo call rates. Override via
 * SNESRECOMP_BOUNDARY_RING_ENTRIES (decimal, clamped to [1<<14, 1<<24]).
 */
#define BOUNDARY_RING_DEFAULT_ENTRIES (1ULL << 20)

#if SNESRECOMP_TRACE
extern BoundaryEvent *g_boundary_ring;
extern uint64_t       g_boundary_capacity;  /* always pow2; mask = cap - 1 */
extern uint64_t       g_boundary_idx;       /* monotonic; modulo via mask */

uint64_t boundary_audit_init(void);
/* Record an entry. `name` is the function name about to begin; the
 * recomp stack has ALREADY been pushed (so g_recomp_stack_top is the
 * post-push depth). Cheap — single atomic-ish increment + memcpy. */
void boundary_audit_record_entry(const char *name);
/* Record an exit. `name` is the function name about to end; the recomp
 * stack has NOT YET been popped (so g_recomp_stack_top is the pre-pop
 * depth). Pairs with the matching entry via the per-call entry_seq
 * stack maintained inline. */
void boundary_audit_record_exit(const char *name);
/* Tag the NEXT boundary EXIT with a non-NORMAL kind. Codegen emits
 * `cpu_trace_mark_nlr_exit(BD_EXIT_KIND_NLR_PRIMARY)` from the Return
 * op when pending_skip != 0, and `cpu_trace_mark_nlr_exit(
 * BD_EXIT_KIND_SKIP_PROPAGATION)` from JSR/JSL callsites that
 * propagate SKIP_N upward. The flag is consumed (and reset to
 * NORMAL) by the next boundary_audit_record_exit. */
void cpu_trace_mark_nlr_exit(uint8_t kind);
#endif

/* ── DB→<value> one-shot tripwire ──────────────────────────────────────
 *
 * Fires the FIRST time cpu->DB transitions from a non-target value to
 * `target_db`. Captures full state, the most recent N boundary events
 * (entries+exits), and the last 64 dbpb events into a structured
 * snapshot. Distinct from `cpu_trace_set_db_watch(b, 1)` which dumps to
 * stderr; this captures structured data for TCP readback and pairs the
 * trip event to the boundary-audit ring index for surrounding-context
 * queries.
 *
 * Dominant SMW symptom 2026-05-02: cpu->DB = $C0 at every
 * ProcessGameMode entry. This tripwire pinpoints the first such
 * corruption on the attract path, with full call-history context.
 */
#define DB_TRIP_BOUNDARY_HISTORY 256
#define DB_TRIP_DBPB_HISTORY      64
#define DB_TRIP_FUNC_LEN          BOUNDARY_NAME_LEN
#define DB_TRIP_STACK_DEPTH       16

typedef struct DbTripwire {
    uint8_t  armed;
    uint8_t  triggered;
    uint8_t  target_db;          /* the DB value that fires the trip */
    uint8_t  pad0;

    /* Trigger metadata */
    int32_t  frame;
    uint32_t trip_pc24;          /* pc24 of the DB-write event */
    uint8_t  old_db;
    uint8_t  new_db;
    uint8_t  trip_event_type;    /* PLB / PLP / RTI / DB_WRITE / ... */
    uint8_t  pad1;
    uint64_t trip_boundary_seq;  /* g_boundary_idx at trip */
    uint64_t trip_trace_idx;     /* g_cpu_trace_idx at trip */

    /* Full CpuState at trip */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    uint8_t  pad2[2];

    /* Most-recent function context */
    char     last_func[DB_TRIP_FUNC_LEN];

    /* Recomp stack at trip (deepest at [stack_depth-1]) */
    int32_t  stack_depth;
    char     stack[DB_TRIP_STACK_DEPTH][DB_TRIP_FUNC_LEN];

    /* Captured history (frozen at trip; not affected by post-trip
     * boundary/dbpb activity). */
    int32_t  bd_count;           /* up to DB_TRIP_BOUNDARY_HISTORY */
    BoundaryEvent bd_history[DB_TRIP_BOUNDARY_HISTORY];

    int32_t  dbpb_count;         /* up to DB_TRIP_DBPB_HISTORY */
    CpuDbpbEvent dbpb_history[DB_TRIP_DBPB_HISTORY];
} DbTripwire;

/* ── Stack-drift tripwire (post-frame-N) ──────────────────────────────
 *
 * Fires on the FIRST function exit where:
 *   - frame >= frame_min (gate; default 400 to skip boot prolog)
 *   - exit_kind == BD_EXIT_KIND_NORMAL (ignore intentional NLR cascade)
 *   - S_exit != S_entry (paired via boundary auditor's entry_seq stack)
 *
 * Auto-arms at boot via cpu_trace_arm_default_watches. Freezes the
 * boundary ring on fire so post-trip activity can't overwrite the
 * imbalance window — same pattern as DB→$C0 tripwire.
 *
 * Captures full state at trip + last 256 boundary events + recomp
 * stack at trip time. Distinct from DB tripwire (which fires on a DB
 * value), this fires on a STRUCTURAL invariant violation (function
 * entry-S != exit-S), catching the next-class-of-NLR-or-stack bug
 * that may surface at the Yoshi-block freeze.
 */
#define STACK_DRIFT_TRIP_BD_HISTORY 256
#define STACK_DRIFT_TRIP_FUNC_LEN BOUNDARY_NAME_LEN
#define STACK_DRIFT_TRIP_STACK_DEPTH 16

typedef struct StackDriftTripwire {
    uint8_t  armed;
    uint8_t  triggered;
    uint8_t  pad[2];
    int32_t  frame_min;        /* trigger only on frame >= this */

    /* Captured at trip */
    int32_t  frame;
    int32_t  s_delta;          /* exit_S - entry_S (signed) */
    uint16_t entry_S;
    uint16_t exit_S;
    uint64_t entry_seq;
    uint64_t exit_seq;
    char     func_name[STACK_DRIFT_TRIP_FUNC_LEN];

    /* Full CpuState at trip */
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    uint8_t  pad2[2];

    /* Recomp stack snapshot */
    int32_t  stack_depth;
    char     stack[STACK_DRIFT_TRIP_STACK_DEPTH][STACK_DRIFT_TRIP_FUNC_LEN];

    /* Frozen boundary history at trip */
    int32_t  bd_count;
    int32_t  pad3;
    BoundaryEvent bd_history[STACK_DRIFT_TRIP_BD_HISTORY];
} StackDriftTripwire;

#if SNESRECOMP_TRACE
extern DbTripwire g_db_tripwire;
extern StackDriftTripwire g_stack_drift_tripwire;

void cpu_trace_arm_db_tripwire(uint8_t target_db);
void cpu_trace_disarm_db_tripwire(void);
/* Internal — fires from cpu_trace_db_change when the trip condition is
 * met. Called unconditionally on every DB change; the armed/triggered
 * gate is checked inline. */
void cpu_trace_db_tripwire_check(CpuState *cpu, uint32_t pc24,
                                 uint8_t old_db, uint8_t new_db,
                                 uint8_t event_type);

/* Arm the stack-drift tripwire. `frame_min` skips imbalances earlier
 * than this frame number (lets the boot prolog complete without
 * spurious trips). */
void cpu_trace_arm_stack_drift_tripwire(int32_t frame_min);
void cpu_trace_disarm_stack_drift_tripwire(void);
/* Internal — called from boundary_audit_record_exit AFTER the EXIT
 * event has been written, with paired entry_S looked up from the
 * active-call stack. */
void cpu_trace_stack_drift_check(uint16_t entry_S, uint16_t exit_S,
                                 uint64_t entry_seq, uint64_t exit_seq,
                                 const char *func_name,
                                 uint8_t exit_kind);
#else
static inline void cpu_trace_arm_db_tripwire(uint8_t b) { (void)b; }
static inline void cpu_trace_disarm_db_tripwire(void) { }
static inline void cpu_trace_db_tripwire_check(CpuState *c, uint32_t p,
                                               uint8_t o, uint8_t n,
                                               uint8_t e) {
    (void)c; (void)p; (void)o; (void)n; (void)e;
}
static inline void cpu_trace_arm_stack_drift_tripwire(int32_t f) { (void)f; }
static inline void cpu_trace_disarm_stack_drift_tripwire(void) { }
static inline void cpu_trace_stack_drift_check(uint16_t es, uint16_t xs,
                                               uint64_t a, uint64_t b,
                                               const char *n, uint8_t k) {
    (void)es; (void)xs; (void)a; (void)b; (void)n; (void)k;
}
static inline uint64_t boundary_audit_init(void) { return 0; }
static inline void boundary_audit_record_entry(const char *n) { (void)n; }
static inline void boundary_audit_record_exit(const char *n) { (void)n; }
static inline void cpu_trace_mark_nlr_exit(uint8_t k) { (void)k; }
#endif

/* ── NLR diagnostic counters (non-rotating) ────────────────────────────
 *
 * Phase 2 of the non-local-return work (2026-05-02) introduced a
 * regression that the rotating cpu_trace ring couldn't characterize
 * — by the time TCP probes attached and queried, NLR_DETECT trace
 * events had already rotated out of the 16M-entry ring (~12 rotations
 * in the first 10 seconds at typical event rates).
 *
 * These monotonic counters survive ring rotation: they answer
 * "did this event ever happen, ever, even once" rather than "did it
 * happen in the last 16M events." Critical for proving whether the
 * NLR-modified blocks actually execute or not — a non-zero
 * site_exec_count is proof of execution; a zero count after Mario
 * has run for many seconds is proof of non-execution.
 */
#define NLR_DIAG_FIRST_FUNC_LEN 64
#define NLR_DIAG_PER_SITE_MAX  16

typedef struct NlrDiag {
    uint64_t site_exec_count;          /* total NLR block executions */
    uint64_t pending_skip_writes;      /* count of cpu->pending_skip = nonzero */
    uint64_t pending_skip_reads_zero;  /* Returns that read pending_skip == 0 */
    uint64_t pending_skip_reads_nonzero; /* Returns that read pending_skip != 0 */

    /* First time pending_skip went from 0 → nonzero, where? */
    uint8_t  first_writer_captured;
    uint8_t  pad[3];
    uint32_t first_writer_pc24;
    int32_t  first_writer_frame;
    uint8_t  first_writer_value;
    uint8_t  pad2[3];
    char     first_writer_func[NLR_DIAG_FIRST_FUNC_LEN];

    /* First time a Return read pending_skip != 0, where? */
    uint8_t  first_consumer_captured;
    uint8_t  pad3[3];
    uint32_t first_consumer_pc24;
    int32_t  first_consumer_frame;
    uint8_t  first_consumer_value;
    uint8_t  pad4[3];
    char     first_consumer_func[NLR_DIAG_FIRST_FUNC_LEN];

    /* Per-site exec counts, keyed by FNV-1a hash of "func/label". */
    int32_t  per_site_used;
    int32_t  pad5;
    uint32_t per_site_hash[NLR_DIAG_PER_SITE_MAX];
    uint64_t per_site_count[NLR_DIAG_PER_SITE_MAX];
    char     per_site_label[NLR_DIAG_PER_SITE_MAX][NLR_DIAG_FIRST_FUNC_LEN];
} NlrDiag;

#if SNESRECOMP_TRACE
extern NlrDiag g_nlr_diag;

/* Record execution of a specific NLR pattern site. Called from the
 * generated NLR-block emit BEFORE the pending_skip store. `name` is
 * a "FuncName/L_BLOCK_M1X1" style label so different NLR sites in the
 * same function are distinguishable. */
void cpu_trace_nlr_site_exec(CpuState *cpu, uint32_t pc24, const char *name);

/* Record a write to cpu->pending_skip that transitions it from 0 to
 * non-zero. Called from the NLR-block emit AFTER setting pending_skip. */
void cpu_trace_pending_skip_write(CpuState *cpu, uint32_t pc24,
                                  uint8_t new_value, const char *func);

/* Record a Return that READ pending_skip. Called from _emit_return.
 * Both zero and non-zero reads counted (separately) so the ratio
 * tells us how often skip-propagation actually happens. */
void cpu_trace_pending_skip_consume(CpuState *cpu, uint32_t pc24,
                                    uint8_t value, const char *func);
#else
static inline void cpu_trace_nlr_site_exec(CpuState *c, uint32_t p, const char *n) {
    (void)c; (void)p; (void)n;
}
static inline void cpu_trace_pending_skip_write(CpuState *c, uint32_t p,
                                                uint8_t v, const char *f) {
    (void)c; (void)p; (void)v; (void)f;
}
static inline void cpu_trace_pending_skip_consume(CpuState *c, uint32_t p,
                                                  uint8_t v, const char *f) {
    (void)c; (void)p; (void)v; (void)f;
}
#endif

/* ── GameMode-14 (in-level) per-tick player-state trace ring ───────────
 *
 * One compact row per call into GameMode14_InLevel_00C47E (entry + exit).
 * Captures the high-signal SMW player/sprite/camera state at each tick
 * boundary so a recomp-vs-oracle differ can find the FIRST tick at
 * which any field diverges. Always-on; ring rotates over runs longer
 * than the configured capacity.
 *
 * Fields: read from g_ram[] (canonical WRAM offsets), so the snapshot
 * sees writes from BOTH gen code (which goes through cpu_write*) and
 * any direct g_ram[] writes from hand-bodies / runtime helpers.
 *
 * Pairing: gm14_tick_ordinal increments on each ENTRY. The matching
 * EXIT row carries the same ordinal. The differ pairs ENTRY/EXIT by
 * ordinal across recomp + oracle.
 *
 * Ring size: default 1<<17 = 131,072 rows ≈ 65,536 ticks ≈ 18 minutes
 * @ 60Hz. Override via SNESRECOMP_GM14_TRACE_ENTRIES (clamped pow2 in
 * [1<<14, 1<<22]).
 */
enum {
    GM14_KIND_ENTRY = 0,
    GM14_KIND_EXIT  = 1,
};

typedef struct Gm14TickRow {
    uint64_t tick_ordinal;       /* monotonic; increments on each ENTRY */
    int32_t  host_frame;         /* snes_frame_counter at row */
    uint8_t  kind;               /* GM14_KIND_ENTRY | GM14_KIND_EXIT */
    uint8_t  gamemode;           /* $7E:0100 */
    /* Inputs */
    uint8_t  in_15;              /* $7E:0015 */
    uint8_t  in_16;              /* $7E:0016 */
    uint8_t  in_17;              /* $7E:0017 */
    uint8_t  in_18;              /* $7E:0018 */
    /* Player core */
    uint8_t  st_71;              /* $7E:0071 PlayerState */
    uint8_t  st_77;              /* $7E:0077 blocked/contact flags */
    int8_t   xspeed;             /* $7E:007B */
    int8_t   yspeed;             /* $7E:007D */
    uint16_t pos_x;              /* $7E:0094-0095 */
    uint16_t pos_y;              /* $7E:0096-0097 */
    /* Yoshi mount */
    uint8_t  yoshi_187A;         /* $7E:187A 1=on Yoshi */
    uint8_t  yoshi_18E2;         /* $7E:18E2 yoshi sprite slot */
    uint8_t  yoshi_1888;         /* $7E:1888 cape/yoshi misc */
    uint8_t  scroll_1A;          /* $7E:001A camera X low */
    uint8_t  scroll_1C;          /* $7E:001C camera Y low */
    uint8_t  pad0;
    uint16_t cam_1462;           /* $7E:1462 layer 1 X */
    uint16_t cam_1464;           /* $7E:1464 layer 1 Y */
    /* Sprite slot 9 */
    uint8_t  spr9_status;        /* $7E:14C8+9 */
    uint8_t  spr9_number;        /* $7E:009E+9 */
    uint16_t spr9_x;             /* high<<8 | low: $7E:14E0+9 / $7E:00E4+9 */
    uint16_t spr9_y;             /* high<<8 | low: $7E:14D4+9 / $7E:00D8+9 */
} Gm14TickRow;

/* Default capacity: 1<<17. Sized for ~18 min @ 60Hz × 2 rows/tick. */
#define GM14_TRACE_DEFAULT_ENTRIES (1ULL << 17)

#if SNESRECOMP_TRACE
extern Gm14TickRow *g_gm14_trace_ring;
extern uint64_t     g_gm14_trace_capacity;   /* always pow2; mask = cap - 1 */
extern uint64_t     g_gm14_trace_idx;        /* monotonic; modulo via mask */
extern uint64_t     g_gm14_tick_ordinal;     /* monotonic tick counter */

/* Init the GM14 ring. Called from cpu_trace_init at process start. */
uint64_t cpu_trace_gm14_init(void);
/* Reset the ring (idx=0, tick_ordinal=0). Does not free memory. */
void cpu_trace_gm14_clear(void);
/* Internal — called from boundary_audit_record_entry/exit when the
 * function name matches GameMode14_InLevel_00C47E_M1X1. The check is
 * pointer-equality fast-path with a one-time strcmp seed. */
void cpu_trace_gm14_maybe_record(const char *func_name, uint8_t kind);
#else
static inline uint64_t cpu_trace_gm14_init(void) { return 0; }
static inline void cpu_trace_gm14_clear(void) { }
static inline void cpu_trace_gm14_maybe_record(const char *n, uint8_t k) {
    (void)n; (void)k;
}
#endif

/* ── Block-keyed sampler ───────────────────────────────────────────────
 *
 * Captures register state + a user-specified set of WRAM byte values on
 * every entry to a chosen block PC, into a non-rotating per-slot ring.
 * Distinct from cpu_trace_block (which records every block entry into
 * the rotating main ring). Use this when you need to inspect state INSIDE
 * a function on every visit to a specific block — without polluting the
 * main ring or pausing execution.
 *
 * Why this exists: func_watch / boundary_audit only capture at function
 * entry/exit. cpu_trace_block fires every block but writes to the
 * rotating ring (oldest events evict). For probing "what's $1FE2+X
 * read at this block?" across many visits, this dedicated sampler keeps
 * the slot's hits non-rotating until cleared.
 */
#define BLOCK_WATCH_MAX           16
#define BLOCK_WATCH_ADDRS_MAX     8
#define BLOCK_WATCH_HITS_MAX      32
#define BLOCK_WATCH_STACK_DEPTH   8
#define BLOCK_WATCH_FUNC_LEN      40

typedef struct BlockWatchHit {
    int32_t  frame;
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, m_flag, x_flag, e_flag;
    uint8_t  pad[2];
    uint8_t  vals[BLOCK_WATCH_ADDRS_MAX];   /* WRAM byte at each watched addr */
    int32_t  stack_depth;
    char     stack[BLOCK_WATCH_STACK_DEPTH][BLOCK_WATCH_FUNC_LEN];
} BlockWatchHit;

typedef struct BlockWatch {
    uint8_t  enabled;
    uint8_t  pad[3];
    uint32_t pc24;                                  /* watched block PC */
    int32_t  n_addrs;                               /* 0..BLOCK_WATCH_ADDRS_MAX */
    int32_t  ram_offsets[BLOCK_WATCH_ADDRS_MAX];   /* g_ram offsets; -1 = unused */
    int32_t  max_hits;                              /* stop capturing after this many */
    int32_t  hit_count;                             /* number captured */
    BlockWatchHit hits[BLOCK_WATCH_HITS_MAX];
} BlockWatch;

#if SNESRECOMP_TRACE
extern BlockWatch g_block_watches[BLOCK_WATCH_MAX];
extern uint8_t    g_block_watch_any;

/* Arm a block-watch slot. If a slot already exists for `pc24`, that slot
 * is reset and reused. `ram_offsets` is an array of `n_addrs` g_ram
 * offsets in [0, 0x20000); -1 means "unused" but normally callers pass
 * exactly n_addrs valid offsets. `max_hits` clamped to BLOCK_WATCH_HITS_MAX. */
void cpu_trace_block_watch_arm(uint32_t pc24,
                                const int32_t *ram_offsets,
                                int n_addrs,
                                int max_hits);
/* Clear all block-watches. */
void cpu_trace_block_watch_clear_all(void);
/* Clear one slot. */
void cpu_trace_block_watch_clear_one(int slot);
/* Internal: called from cpu_trace_block on every block entry. Cheap when
 * no watch is armed (one bool check). */
void cpu_trace_block_watch_check(CpuState *cpu, uint32_t pc24);
#else
static inline void cpu_trace_block_watch_arm(uint32_t p, const int32_t *o, int n, int m) {
    (void)p; (void)o; (void)n; (void)m;
}
static inline void cpu_trace_block_watch_clear_all(void) { }
static inline void cpu_trace_block_watch_clear_one(int s) { (void)s; }
static inline void cpu_trace_block_watch_check(CpuState *c, uint32_t p) { (void)c; (void)p; }
#endif

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
static inline void cpu_trace_wram_write_check(CpuState *c, uint8_t b, uint16_t a, int32_t off, uint16_t ov, uint16_t nv, int w) { (void)c; (void)b; (void)a; (void)off; (void)ov; (void)nv; (void)w; }
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
static inline void cpu_trace_stack_op(CpuState *c, uint32_t p, uint8_t op,
                                      uint16_t s, int8_t d) {
    (void)c; (void)p; (void)op; (void)s; (void)d;
}
static inline void cpu_trace_phantom_arm(uint32_t p, const char *l)         { (void)p; (void)l; }
static inline void cpu_trace_phantom_disarm_all(void)                       { }
static inline void cpu_trace_phantom_arm_smc_set(void)                      { }
static inline void cpu_trace_phantom_arm_unresolvable_goto_set(void)        { }
static inline RecompReturn cpu_trace_unresolved_goto_trap(
    CpuState *c, uint32_t s, uint32_t t, const char *fn, const char *lbl) {
    (void)c; (void)s; (void)t; (void)fn; (void)lbl;
    return RECOMP_RETURN_NORMAL;
}
static inline RecompReturn cpu_trace_unresolved_stub_trap(
    CpuState *c, uint32_t t, const char *fn) {
    (void)c; (void)t; (void)fn;
    return RECOMP_RETURN_NORMAL;
}

#endif

#ifdef __cplusplus
}
#endif
