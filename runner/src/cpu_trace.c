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
    e->native_func_id_or_hash = name ? fnv1a(name) : 0;
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
        }
        fprintf(stderr, "\n");
    }
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
