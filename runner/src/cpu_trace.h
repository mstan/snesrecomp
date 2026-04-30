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

#define CPU_TRACE_RING_LEN  4096
#define CPU_DBPB_RING_LEN   64

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
void cpu_trace_clear(void);

/* Dump the last `n` events of the main ring to stderr, prefixed by `tag`. */
void cpu_trace_dump_recent(const char *tag, int n);
/* Dump the entire dbpb ring (newest first). */
void cpu_trace_dump_dbpb(const char *tag);

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
static inline void cpu_trace_clear(void)                                    { }
static inline void cpu_trace_dump_recent(const char *tag, int n)            { (void)tag; (void)n; }
static inline void cpu_trace_dump_dbpb(const char *tag)                     { (void)tag; }

#endif

#ifdef __cplusplus
}
#endif
