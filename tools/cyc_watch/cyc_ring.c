#include "cyc_ring.h"
#include <stdlib.h>
#include <string.h>

static int is_pow2(size_t n) { return n && ((n & (n - 1)) == 0); }

void cyc_ring_init(CycRing *r, size_t capacity_pow2) {
    if (!is_pow2(capacity_pow2)) {
        /* round up to next power of two */
        size_t c = 1;
        while (c < capacity_pow2) c <<= 1;
        capacity_pow2 = c ? c : 1;
    }
    r->buf = (CycRec *)calloc(capacity_pow2, sizeof(CycRec));
    r->cap = capacity_pow2;
    r->total = 0;
}

void cyc_ring_free(CycRing *r) {
    free(r->buf);
    r->buf = NULL;
    r->cap = 0;
    r->total = 0;
}

void cyc_ring_push(CycRing *r, uint32_t pc24, uint8_t opcode,
                   uint16_t cyc_auth, uint16_t cyc_ref, uint32_t master) {
    CycRec *rec = &r->buf[r->total & (r->cap - 1)];
    rec->seq = r->total;
    rec->pc24 = pc24;
    rec->opcode = opcode;
    rec->cyc_auth = cyc_auth;
    rec->cyc_ref = cyc_ref;
    rec->master = master;
    r->total++;
}

uint64_t cyc_ring_oldest(const CycRing *r) {
    return (r->total > r->cap) ? (r->total - r->cap) : 0;
}

const CycRec *cyc_ring_get(const CycRing *r, uint64_t seq) {
    if (seq >= r->total) return NULL;             /* not written yet */
    if (seq < cyc_ring_oldest(r)) return NULL;    /* evicted */
    return &r->buf[seq & (r->cap - 1)];
}

uint64_t cyc_ring_find_pc(const CycRing *r, uint32_t pc24, uint64_t from_seq) {
    uint64_t s = from_seq;
    uint64_t oldest = cyc_ring_oldest(r);
    if (s < oldest) s = oldest;
    for (; s < r->total; s++) {
        if (r->buf[s & (r->cap - 1)].pc24 == pc24) return s;
    }
    return CYC_SEQ_NONE;
}

CycRegion cyc_ring_region(const CycRing *r, uint64_t start_seq, uint64_t end_seq) {
    CycRegion out = {0, 0, 0, 0};
    for (uint64_t s = start_seq; s < end_seq; s++) {
        const CycRec *rec = cyc_ring_get(r, s);
        if (!rec) break;                          /* evicted / out of range */
        out.count++;
        out.sum_auth += rec->cyc_auth;
        out.sum_ref += rec->cyc_ref;
        out.sum_master += rec->master;
    }
    return out;
}

CycRegion cyc_ring_region_anchors(const CycRing *r, uint32_t start_pc,
                                  uint32_t end_pc, uint64_t from_seq) {
    CycRegion empty = {0, 0, 0, 0};
    uint64_t s0 = cyc_ring_find_pc(r, start_pc, from_seq);
    if (s0 == CYC_SEQ_NONE) return empty;
    uint64_t s1 = cyc_ring_find_pc(r, end_pc, s0 + 1);
    if (s1 == CYC_SEQ_NONE) return empty;
    return cyc_ring_region(r, s0, s1);
}
