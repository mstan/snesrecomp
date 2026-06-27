/*
 * cyc_ring -- always-on per-instruction cycle ring buffer (Axis 2 / cyc_watch).
 *
 * Records EVERY executed instruction from boot into a bounded circular buffer
 * (eviction keeps memory flat). Probes QUERY a window after the fact; they
 * never arm-then-capture (PRINCIPLES.md ring-buffer discipline). Each record
 * holds both the shared-authority cycle count and the reference engine's
 * native count, so a window can be diffed model-vs-model, and the two-anchor
 * REGION query measures the cycle delta of one START->END pass over a known
 * code path (the absolute offset cancels).
 */
#ifndef CYC_RING_H
#define CYC_RING_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t seq;        /* monotonic instruction index */
    uint32_t pc24;       /* guest PB:PC of the instruction */
    uint8_t  opcode;
    uint16_t cyc_auth;   /* shared-authority CPU (bus) cycles */
    uint16_t cyc_ref;    /* reference engine's native CPU cycles */
    uint32_t master;     /* authority master-clock cycles (region-weighted) */
} CycRec;

typedef struct {
    CycRec  *buf;
    size_t   cap;        /* power of two */
    uint64_t total;      /* records ever pushed; also the next seq */
} CycRing;

typedef struct {
    uint64_t count;
    uint64_t sum_auth;
    uint64_t sum_ref;
    uint64_t sum_master;
} CycRegion;

#define CYC_SEQ_NONE UINT64_MAX

void   cyc_ring_init(CycRing *r, size_t capacity_pow2);
void   cyc_ring_free(CycRing *r);
void   cyc_ring_push(CycRing *r, uint32_t pc24, uint8_t opcode,
                     uint16_t cyc_auth, uint16_t cyc_ref, uint32_t master);

/* Oldest seq still resident (older ones were evicted). */
uint64_t cyc_ring_oldest(const CycRing *r);
/* Record by seq, or NULL if evicted / not yet written. */
const CycRec *cyc_ring_get(const CycRing *r, uint64_t seq);

/* First seq >= from_seq whose pc24 matches, or CYC_SEQ_NONE. */
uint64_t cyc_ring_find_pc(const CycRing *r, uint32_t pc24, uint64_t from_seq);

/* Sum over [start_seq, end_seq). Stops early if a record was evicted. */
CycRegion cyc_ring_region(const CycRing *r, uint64_t start_seq, uint64_t end_seq);

/* Two-anchor REGION: the cycle cost of one pass from the first crossing of
 * `start_pc` (at/after from_seq) to the first crossing of `end_pc` after it.
 * If start_pc == end_pc, measures one full pass between consecutive crossings
 * (e.g. a single loop iteration). Returns {0,...} if anchors not found. */
CycRegion cyc_ring_region_anchors(const CycRing *r, uint32_t start_pc,
                                  uint32_t end_pc, uint64_t from_seq);

#endif /* CYC_RING_H */
