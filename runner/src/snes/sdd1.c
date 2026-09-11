/*
 * S-DD1 chip emulation — faithful port of bsnes-plus (Andreas Naive algorithm).
 *
 * Two entry points:
 *   sdd1_decompress()        — whole-block decompression (CPU reads via $C0-$FF)
 *   sdd1_dma_get_byte()      — streaming byte-at-a-time (DMA transfer)
 *
 * Both use the same Golomb / PEM / CM / OL engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "sdd1.h"
#include "saveload.h"

/* Debug logging gate: set SNESRECOMP_SDD1_DEBUG=1 to enable the per-byte
 * decompression trace. Off by default — the trace writes megabytes per
 * second (Star Ocean decompresses hundreds of blocks per frame), so it is
 * only useful for focused diagnostics on a small number of frames. */
#ifdef SNESRECOMP_INTERP_PROFILE
uint64_t sdd1_prof_blocks = 0;
uint64_t sdd1_prof_bytes = 0;
double sdd1_prof_ms = 0.0;
#endif

static int sdd1_debug_enabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("SNESRECOMP_SDD1_DEBUG") ? 1 : 0;
    return v;
}

/* ---- bsnes run_count[256] table (from sdd1emu.cpp) ---- */

static const uint8_t run_count_table[256] = {
    0x00, 0x00, 0x01, 0x00, 0x03, 0x01, 0x02, 0x00,
    0x07, 0x03, 0x05, 0x01, 0x06, 0x02, 0x04, 0x00,
    0x0f, 0x07, 0x0b, 0x03, 0x0d, 0x05, 0x09, 0x01,
    0x0e, 0x06, 0x0a, 0x02, 0x0c, 0x04, 0x08, 0x00,
    0x1f, 0x0f, 0x17, 0x07, 0x1b, 0x0b, 0x13, 0x03,
    0x1d, 0x0d, 0x15, 0x05, 0x19, 0x09, 0x11, 0x01,
    0x1e, 0x0e, 0x16, 0x06, 0x1a, 0x0a, 0x12, 0x02,
    0x1c, 0x0c, 0x14, 0x04, 0x18, 0x08, 0x10, 0x00,
    0x3f, 0x1f, 0x2f, 0x0f, 0x37, 0x17, 0x27, 0x07,
    0x3b, 0x1b, 0x2b, 0x0b, 0x33, 0x13, 0x23, 0x03,
    0x3d, 0x1d, 0x2d, 0x0d, 0x35, 0x15, 0x25, 0x05,
    0x39, 0x19, 0x29, 0x09, 0x31, 0x11, 0x21, 0x01,
    0x3e, 0x1e, 0x2e, 0x0e, 0x36, 0x16, 0x26, 0x06,
    0x3a, 0x1a, 0x2a, 0x0a, 0x32, 0x12, 0x22, 0x02,
    0x3c, 0x1c, 0x2c, 0x0c, 0x34, 0x14, 0x24, 0x04,
    0x38, 0x18, 0x28, 0x08, 0x30, 0x10, 0x20, 0x00,
    0x7f, 0x3f, 0x5f, 0x1f, 0x6f, 0x2f, 0x4f, 0x0f,
    0x77, 0x37, 0x57, 0x17, 0x67, 0x27, 0x47, 0x07,
    0x7b, 0x3b, 0x5b, 0x1b, 0x6b, 0x2b, 0x4b, 0x0b,
    0x73, 0x33, 0x53, 0x13, 0x63, 0x23, 0x43, 0x03,
    0x7d, 0x3d, 0x5d, 0x1d, 0x6d, 0x2d, 0x4d, 0x0d,
    0x75, 0x35, 0x55, 0x15, 0x65, 0x25, 0x45, 0x05,
    0x79, 0x39, 0x59, 0x19, 0x69, 0x29, 0x49, 0x09,
    0x71, 0x31, 0x51, 0x11, 0x61, 0x21, 0x41, 0x01,
    0x7e, 0x3e, 0x5e, 0x1e, 0x6e, 0x2e, 0x4e, 0x0e,
    0x76, 0x36, 0x56, 0x16, 0x66, 0x26, 0x46, 0x06,
    0x7a, 0x3a, 0x5a, 0x1a, 0x6a, 0x2a, 0x4a, 0x0a,
    0x72, 0x32, 0x52, 0x12, 0x62, 0x22, 0x42, 0x02,
    0x7c, 0x3c, 0x5c, 0x1c, 0x6c, 0x2c, 0x4c, 0x0c,
    0x74, 0x34, 0x54, 0x14, 0x64, 0x24, 0x44, 0x04,
    0x78, 0x38, 0x58, 0x18, 0x68, 0x28, 0x48, 0x08,
    0x70, 0x30, 0x50, 0x10, 0x60, 0x20, 0x40, 0x00,
};

/* ---- Evolution table (probability estimation) ---- */

static const struct {
    uint8_t code_size;
    uint8_t MPS_next;
    uint8_t LPS_next;
} evolution_table[] = {
    /*  0 */ { 0, 25, 25 },
    /*  1 */ { 0,  2,  1 },
    /*  2 */ { 0,  3,  1 },
    /*  3 */ { 0,  4,  2 },
    /*  4 */ { 0,  5,  3 },
    /*  5 */ { 1,  6,  4 },
    /*  6 */ { 1,  7,  5 },
    /*  7 */ { 1,  8,  6 },
    /*  8 */ { 1,  9,  7 },
    /*  9 */ { 2, 10,  8 },
    /* 10 */ { 2, 11,  9 },
    /* 11 */ { 2, 12, 10 },
    /* 12 */ { 2, 13, 11 },
    /* 13 */ { 3, 14, 12 },
    /* 14 */ { 3, 15, 13 },
    /* 15 */ { 3, 16, 14 },
    /* 16 */ { 3, 17, 15 },
    /* 17 */ { 4, 18, 16 },
    /* 18 */ { 4, 19, 17 },
    /* 19 */ { 5, 20, 18 },
    /* 20 */ { 5, 21, 19 },
    /* 21 */ { 6, 22, 20 },
    /* 22 */ { 6, 23, 21 },
    /* 23 */ { 7, 24, 22 },
    /* 24 */ { 7, 24, 23 },
    /* 25 */ { 0, 26,  1 },
    /* 26 */ { 1, 27,  2 },
    /* 27 */ { 2, 28,  4 },
    /* 28 */ { 3, 29,  8 },
    /* 29 */ { 4, 30, 12 },
    /* 30 */ { 5, 31, 16 },
    /* 31 */ { 6, 32, 18 },
    /* 32 */ { 7, 24, 22 },
};

/* ---- MMC (banks C0-FF, four 1MB pages selected by $4804-$4807) ---- */

static uint32_t sdd1_mmc_linear(const Sdd1 *sdd1, uint32_t addr24) {
    uint8_t page;
    switch ((addr24 >> 20) & 3) {
        case 0:  page = sdd1->r4804; break;
        case 1:  page = sdd1->r4805; break;
        case 2:  page = sdd1->r4806; break;
        default: page = sdd1->r4807; break;
    }
    return ((uint32_t)(page & 0x0f) << 20) | (addr24 & 0xFFFFF);
}

static uint8_t sdd1_mmc_read_byte(const Sdd1 *sdd1, uint32_t addr24) {
    if (!sdd1 || !sdd1->rom || sdd1->rom_size == 0) return 0;
    uint32_t off = sdd1_mmc_linear(sdd1, addr24);
    if (sdd1_debug_enabled()) {
      static int mmc_log = 0;
      if (mmc_log < 50) {
        fprintf(stderr, "[SDD1_MMC_READ] addr24=%06X -> linear_off=%06X rom_size=%u byte=%02X (r4804=%02X r4805=%02X r4806=%02X r4807=%02X)\n",
                addr24, off, sdd1->rom_size,
                off < sdd1->rom_size ? sdd1->rom[off] : 0xFF,
                sdd1->r4804, sdd1->r4805, sdd1->r4806, sdd1->r4807);
        mmc_log++;
      }
    }
    return off < sdd1->rom_size ? sdd1->rom[off] : 0;
}

/* ---- Unified decompression engine ----
 *
 * Faithful port of bsnes-plus sdd1emu.cpp.  Works for both whole-block
 * and streaming DMA modes.  The only difference is how bytes are read
 * from the compressed stream (buffer vs. MMC window). */

/* Callback for reading the next byte from the compressed stream */
#ifdef SNESRECOMP_INTERP_PROFILE
#include <time.h>
#endif
typedef uint8_t (*sdd1_read_cb)(void *ctx);

/* All decompression state lives in this struct (no globals). */
typedef struct {
    sdd1_read_cb read_byte;
    void *read_ctx;

    /* For whole-block: pointer to compressed data buffer */
    const uint8_t *rom_ptr;
    /* For streaming: S-DD1 chip pointer for MMC reads */
    Sdd1 *stream_sdd1;

    /* IM (Input Manager) */
    uint32_t byte_ptr;      /* current byte position in source */
    uint8_t  bit_count;     /* bits consumed in current byte (0-7) */

    /* BG (Bit Generator) - one per code_num (0-7) */
    uint8_t  MPScount[8];
    uint8_t  LPSind[8];

    /* PEM (Probability Estimation) */
    uint8_t  context_states[32];
    uint8_t  context_MPS[32];

    /* CM (Context Model) */
    uint8_t  bitplane_type;
    uint8_t  contextBitsInfo;
    uint8_t  currBitplane;
    uint16_t prevBitplaneBits[8];
    uint16_t bit_number;

    /* OL (Output Logic) */
    uint16_t length;
} Sdd1Decomp;

/* ---- IM: getCodeword ---- */

static uint8_t sdd1_getCodeword(Sdd1Decomp *d, uint8_t code_len) {
    uint8_t codeword;

    /* Read the current byte shifted left by bit_count */
    uint8_t byte0 = d->read_byte(d->read_ctx);
    uint32_t byte_ptr_at_start = d->byte_ptr;
    codeword = byte0 << d->bit_count;
    d->bit_count++;

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_GCR_DEBUG] byte_ptr=%u byte0=%02X bit_count=%u code_len=%u codeword_partial=%02X\n",
              byte_ptr_at_start, byte0, d->bit_count, code_len, codeword);
    }

    if (codeword & 0x80) {
        /* Escape: also read bits from the NEXT byte (byte_ptr+1) */
        uint32_t saved = d->byte_ptr;
        d->byte_ptr = (saved + 1) & 0xFFFFFF;
        uint8_t byte1 = d->read_byte(d->read_ctx);
        codeword |= byte1 >> (9 - d->bit_count);
        d->byte_ptr = saved;
        d->bit_count += code_len;
        if (sdd1_debug_enabled()) {
          fprintf(stderr, "[SDD1_GCR] byte0=%02X byte1=%02X bit_count=%u code_len=%u codeword=%02X ESCAPE\n",
                  byte0, byte1, d->bit_count, code_len, codeword);
        }
    } else if (sdd1_debug_enabled()) {
        fprintf(stderr, "[SDD1_GCR] byte0=%02X bit_count=%u code_len=%u codeword=%02X\n",
                byte0, d->bit_count, code_len, codeword);
    }

    if (d->bit_count & 0x08) {
        d->byte_ptr++;
        d->bit_count &= 0x07;
    }

    return codeword;
}

/* ---- GCD: getRunCount ---- */

static void sdd1_getRunCount(Sdd1Decomp *d, uint8_t code_num,
                             uint8_t *MPScount, uint8_t *LPSind) {
    uint8_t codeword = sdd1_getCodeword(d, code_num);

    if (codeword & 0x80) {
        *LPSind = 1;
        *MPScount = run_count_table[codeword >> (code_num ^ 0x07)];
        if (sdd1_debug_enabled()) {
          fprintf(stderr, "[SDD1_GRC] code_num=%u codeword=%02X -> LPSind=1 MPScount=%u (table_idx=%u)\n",
                  code_num, codeword, *MPScount, codeword >> (code_num ^ 0x07));
        }
    } else {
        *LPSind = 0;
        *MPScount = (uint8_t)(1 << code_num);
        if (sdd1_debug_enabled()) {
          fprintf(stderr, "[SDD1_GRC] code_num=%u codeword=%02X -> LPSind=0 MPScount=%u\n",
                    code_num, codeword, *MPScount);
        }
    }
}

/* ---- BG: getBit ---- */

static uint8_t sdd1_bg_getBit(Sdd1Decomp *d, uint8_t code_num,
                               uint8_t *endOfRun) {
    uint8_t bit;

    uint8_t pre_MPS = d->MPScount[code_num];
    uint8_t pre_LPS = d->LPSind[code_num];
    bool will_call_getRunCount = !(pre_MPS || pre_LPS);

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_BG_PRE] code_num=%u MPScount(BEFORE)=%u LPSind(BEFORE)=%u will_call_getRunCount=%u\n",
              code_num, pre_MPS, pre_LPS, will_call_getRunCount);
    }

    if (will_call_getRunCount) {
        sdd1_getRunCount(d, code_num, &d->MPScount[code_num], &d->LPSind[code_num]);
    }

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_BG_MID] code_num=%u MPScount(AFTER)=%u LPSind(AFTER)=%u\n",
              code_num, d->MPScount[code_num], d->LPSind[code_num]);
    }

    if (d->MPScount[code_num]) {
        bit = 0;  /* MPS bit */
        d->MPScount[code_num]--;
    } else {
        bit = 1;  /* LPS bit */
        d->LPSind[code_num] = 0;
    }

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_BG_POST] code_num=%u MPScount=%u LPSind=%u -> bit=%u\n",
              code_num, d->MPScount[code_num], d->LPSind[code_num], bit);
    }

    if (d->MPScount[code_num] || d->LPSind[code_num])
        *endOfRun = 0;
    else
        *endOfRun = 1;

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_BG] code_num=%u MPScount=%u LPSind=%u -> bit=%u endOfRun=%u\n",
              code_num, d->MPScount[code_num], d->LPSind[code_num], bit, *endOfRun);
    }

    return bit;
}

/* ---- PEM: getBit ---- */

static uint8_t sdd1_pem_getBit(Sdd1Decomp *d, uint8_t context) {
    uint8_t endOfRun;
    uint8_t bit;
    uint8_t currStatus = d->context_states[context];
    const uint8_t code_num = evolution_table[currStatus].code_size;
    uint8_t currentMPS = d->context_MPS[context];

    bit = sdd1_bg_getBit(d, code_num, &endOfRun);

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_PEM] context=%u currStatus=%u code_num=%u currentMPS=%u bit=%u endOfRun=%u\n",
              context, currStatus, code_num, currentMPS, bit, endOfRun);
    }

    if (endOfRun) {
        if (bit) {
            /* LPS bit */
            if (!(currStatus & 0xfe))
                d->context_MPS[context] ^= 0x01;
            d->context_states[context] = evolution_table[currStatus].LPS_next;
        } else {
            /* MPS bit */
            d->context_states[context] = evolution_table[currStatus].MPS_next;
        }
    }

    return bit ^ currentMPS;
}

/* ---- CM: getBit ---- */

static uint8_t sdd1_cm_getBit(Sdd1Decomp *d) {
    uint8_t currContext;
    uint16_t *context_bits;

    switch (d->bitplane_type) {
    case 0x00:
        d->currBitplane ^= 0x01;
        break;
    case 0x40:
        d->currBitplane ^= 0x01;
        if (!(d->bit_number & 0x7f))
            d->currBitplane = (uint8_t)((d->currBitplane + 2) & 0x07);
        break;
    case 0x80:
        d->currBitplane ^= 0x01;
        if (!(d->bit_number & 0x7f))
            d->currBitplane ^= 0x02;
        break;
    case 0xc0:
        d->currBitplane = (uint8_t)(d->bit_number & 0x07);
        break;
    }

    context_bits = &d->prevBitplaneBits[d->currBitplane];

    currContext = (uint8_t)((d->currBitplane & 0x01) << 4);
    switch (d->contextBitsInfo) {
    case 0x00:
        currContext |= (uint8_t)(((*context_bits & 0x01c0) >> 5) | (*context_bits & 0x0001));
        break;
    case 0x10:
        currContext |= (uint8_t)(((*context_bits & 0x0180) >> 5) | (*context_bits & 0x0001));
        break;
    case 0x20:
        currContext |= (uint8_t)(((*context_bits & 0x00c0) >> 5) | (*context_bits & 0x0001));
        break;
    case 0x30:
        currContext |= (uint8_t)(((*context_bits & 0x0180) >> 5) | (*context_bits & 0x0003));
        break;
    }

    uint8_t bit = sdd1_pem_getBit(d, currContext);

    if (sdd1_debug_enabled()) {
      fprintf(stderr, "[SDD1_CM] bitplane_type=%02X contextBitsInfo=%02X bit_number=%u currBitplane=%u currContext=%u -> bit=%u\n",
              d->bitplane_type, d->contextBitsInfo, d->bit_number, d->currBitplane, currContext, bit);
    }

    *context_bits = (uint16_t)((*context_bits << 1) | bit);
    d->bit_number++;

    return bit;
}

/* ---- Prepare decompression (called before OL::launch) ---- */

static void sdd1_prepareDecomp(Sdd1Decomp *d) {
    /* bit_count starts at 4 because the first 4 bits of byte 0 are already
     * consumed by the header (bitplane_type and contextBitsInfo).
     * byte_ptr is NOT reset here — the caller sets it to the correct
     * starting position (0 for buffer, src_pos for streaming). */
    d->bit_count = 4;

    /* BG: all MPScount and LPSind start at 0 */
    memset(d->MPScount, 0, sizeof(d->MPScount));
    memset(d->LPSind, 0, sizeof(d->LPSind));

    /* PEM: all contexts start at state 0, MPS 0 */
    memset(d->context_states, 0, sizeof(d->context_states));
    memset(d->context_MPS, 0, sizeof(d->context_MPS));

    /* CM: read header byte 0 */
    uint8_t header0 = d->read_byte(d->read_ctx);
    d->bitplane_type = header0 & 0xc0;
    d->contextBitsInfo = header0 & 0x30;
    d->bit_number = 0;
    memset(d->prevBitplaneBits, 0, sizeof(d->prevBitplaneBits));

    switch (d->bitplane_type) {
    case 0x00: d->currBitplane = 1; break;
    case 0x40: d->currBitplane = 7; break;
    case 0x80: d->currBitplane = 3; break;
    case 0xc0: d->currBitplane = 0; break;
    }

    /* byte 1 is consumed naturally by getCodeword during the first
     * call to getRunCount. No need to read it here. */
}

/* ---- OL: launch (assemble output bytes) ---- */

static void sdd1_ol_launch(Sdd1Decomp *d, uint8_t *out_buf) {
    uint16_t length = d->length;
#ifdef SNESRECOMP_INTERP_PROFILE
    clock_t _t0 = clock();
#endif

    switch (d->bitplane_type) {
    case 0x00:
    case 0x40:
    case 0x80: {
        uint8_t i = 1;
        uint8_t register1, register2;
        do {
            if (!i) {
                *(out_buf++) = register2;
                i = (uint8_t)~i;
            } else {
                for (register1 = register2 = 0, i = 0x80; i; i >>= 1) {
                    if (sdd1_cm_getBit(d)) register1 |= i;
                    if (sdd1_cm_getBit(d)) register2 |= i;
                }
                *(out_buf++) = register1;
            }
        } while (--length);
        break;
    }
    case 0xc0: {
        uint8_t register1, i;
        do {
            for (register1 = 0, i = 0x01; i; i <<= 1) {
                if (sdd1_cm_getBit(d)) register1 |= i;
            }
            *(out_buf++) = register1;
        } while (--length);
        break;
    }
    }
#ifdef SNESRECOMP_INTERP_PROFILE
    { extern uint64_t sdd1_prof_blocks, sdd1_prof_bytes;
      extern double sdd1_prof_ms;
      sdd1_prof_blocks++;
      sdd1_prof_bytes += d->length;
      sdd1_prof_ms += 1000.0 * ((double)(clock() - _t0)) / CLOCKS_PER_SEC;
    }
#endif
}

/* ---- Whole-block decompression ---- */

/* Callback: read from a flat buffer (does NOT advance byte_ptr) */
static uint8_t sdd1_read_from_buf(void *ctx) {
    Sdd1Decomp *d = (Sdd1Decomp *)ctx;
    return d->rom_ptr[d->byte_ptr];
}

void sdd1_decompress(uint8_t* out, const uint8_t* in, int len) {
    Sdd1Decomp d;

    if (len == 0) len = 0x10000;

    memset(&d, 0, sizeof(d));
    d.read_byte = sdd1_read_from_buf;
    d.read_ctx = &d;
    d.rom_ptr = in;
    d.length = (uint16_t)len;

    sdd1_prepareDecomp(&d);
    sdd1_ol_launch(&d, out);

    if (sdd1_debug_enabled()) {
      static int decomp_log = 0;
      if (decomp_log < 10) {
        fprintf(stderr, "[SDD1_DECOMP_TEST] len=%d first16: ", len);
        for (int i = 0; i < 16 && i < len; i++)
          fprintf(stderr, "%02X ", out[i]);
        fprintf(stderr, "\n");
        decomp_log++;
      }
    }
}

/* ---- chip lifecycle ---- */

Sdd1* sdd1_create(const uint8_t* rom, uint32_t rom_size, uint8_t* ram, uint32_t ram_size) {
    (void)ram;
    (void)ram_size;
    Sdd1* s = (Sdd1*)calloc(1, sizeof(Sdd1));
    if (s) {
        s->rom = rom;
        s->rom_size = rom_size;
    }
    return s;
}

void sdd1_destroy(Sdd1* sdd1) {
    if (sdd1) {
        for (int i = 0; i < 8; i++) {
            free(sdd1->dma_state[i].decomp_buf);
            free(sdd1->cpu_ch[i].buf);
        }
    }
    free(sdd1);
}

void sdd1_reset(Sdd1* sdd1) {
    if (!sdd1) return;
    sdd1->r4800 = 0x00;
    sdd1->r4801 = 0x00;
    sdd1->r4804 = 0x00;
    sdd1->r4805 = 0x01;
    sdd1->r4806 = 0x02;
    sdd1->r4807 = 0x03;
    memset(sdd1->dma_state, 0, sizeof(sdd1->dma_state));
    for (int i = 0; i < 8; i++) {
        free(sdd1->cpu_ch[i].buf);
        memset(&sdd1->cpu_ch[i], 0, sizeof(sdd1->cpu_ch[i]));
    }
}

void sdd1_sync(Sdd1* sdd1, uint64_t master_clock) {
    (void)sdd1; (void)master_clock;
}

void sdd1_saveload(Sdd1* sdd1, struct SaveLoadInfo* sli) {
    if (!sdd1 || !sli) return;

    sli->func(sli, &sdd1->r4800, 1);
    sli->func(sli, &sdd1->r4801, 1);
    sli->func(sli, &sdd1->r4804, 1);
    sli->func(sli, &sdd1->r4805, 1);
    sli->func(sli, &sdd1->r4806, 1);
    sli->func(sli, &sdd1->r4807, 1);

    for (int ch = 0; ch < 8; ch++) {
        sli->func(sli, &sdd1->dma_state[ch].active, sizeof(sdd1->dma_state[ch].active));
        sli->func(sli, &sdd1->dma_state[ch].src_addr, sizeof(sdd1->dma_state[ch].src_addr));
        sli->func(sli, &sdd1->dma_state[ch].src_pos, sizeof(sdd1->dma_state[ch].src_pos));
        sli->func(sli, &sdd1->dma_state[ch].bytes_remaining, sizeof(sdd1->dma_state[ch].bytes_remaining));
        sli->func(sli, &sdd1->dma_state[ch].header_read, sizeof(sdd1->dma_state[ch].header_read));
    }
}

/* ---- register interface ($4800-$4807) ---- */

uint8_t sdd1_read(Sdd1* sdd1, uint16_t addr) {
    if (!sdd1) return 0;
    switch (addr & 0xF) {
        case 0x0: return sdd1->r4800;
        case 0x1: return sdd1->r4801;
        case 0x4: return sdd1->r4804;
        case 0x5: return sdd1->r4805;
        case 0x6: return sdd1->r4806;
        case 0x7: return sdd1->r4807;
        default:  return 0;
    }
}

void sdd1_write(Sdd1* sdd1, uint16_t addr, uint8_t val) {
    if (!sdd1) return;
    /* Only log $4800/$4801 (hard/soft enable) — skip noisy $4804-$4807 bank regs */
    if (sdd1_debug_enabled()) {
      static int wr_log_01 = 0;
      uint8_t reg = addr & 0xF;
      if ((reg == 0x0 || reg == 0x1) && wr_log_01 < 30) {
        fprintf(stderr, "[SDD1_WRITE] $480%d=$%02X (raw=$%04X)\n", reg, val, addr);
        wr_log_01++;
      }
    }
    switch (addr & 0xF) {
        case 0x0: sdd1->r4800 = val; break;
        case 0x1:
            sdd1->r4801 = val;
            for (int i = 0; i < 8; i++) {
                bool should_arm = (sdd1->r4800 & sdd1->r4801 & (1u << i)) != 0;
                if (should_arm && !sdd1->cpu_ch[i].armed) {
                    sdd1->cpu_ch[i].armed = true;
                } else if (!should_arm && sdd1->cpu_ch[i].armed) {
                    /* Channel disabled — free any pending decompression buffer */
                    free(sdd1->cpu_ch[i].buf);
                    sdd1->cpu_ch[i].buf = NULL;
                    sdd1->cpu_ch[i].buf_size = 0;
                    sdd1->cpu_ch[i].buf_pos = 0;
                    sdd1->cpu_ch[i].armed = false;
                }
            }
            break;
        case 0x4: sdd1->r4804 = val & 0x8f; break;
        case 0x5: sdd1->r4805 = val & 0x8f; break;
        case 0x6: sdd1->r4806 = val & 0x8f; break;
        case 0x7: sdd1->r4807 = val & 0x8f; break;
        default:  break;
    }
}

/* ---- Streaming DMA decompression (byte-at-a-time) ---- */

/* Callback: read from MMC window via stream_sdd1 (does NOT advance byte_ptr) */
static uint8_t sdd1_read_from_stream(void *ctx) {
    Sdd1Decomp *d = (Sdd1Decomp *)ctx;
    return sdd1_mmc_read_byte(d->stream_sdd1, d->byte_ptr);
}

void sdd1_dma_init(Sdd1* sdd1, int channel, uint32_t src_addr24, uint32_t transfer_size) {
    if (!sdd1 || channel < 0 || channel > 7) return;
    if (!(sdd1->r4800 & sdd1->r4801 & (1u << channel))) return;
    if (!sdd1->rom || sdd1->rom_size == 0) return;

    Sdd1DmaState *state = &sdd1->dma_state[channel];

    /* Free any previous decompression buffer */
    free(state->decomp_buf);
    state->decomp_buf = NULL;
    state->decomp_buf_size = 0;
    state->decomp_pos = 0;

    state->active = true;
    state->src_addr = src_addr24 & 0xFFFFFF;
    state->src_pos = src_addr24 & 0xFFFFFF;
    state->bytes_remaining = transfer_size ? transfer_size : 0x10000;
    state->header_read = false;
}

/* Get next decompressed byte for DMA streaming.
 * Uses the unified bsnes engine: on first call, reads the 2-byte header
 * and prepares the decompression context. On subsequent calls, resumes
 * the context to produce the next output byte. */
uint8_t sdd1_dma_get_byte(Sdd1* sdd1, int channel) {
    if (!sdd1 || channel < 0 || channel > 7) return 0;
    Sdd1DmaState *state = &sdd1->dma_state[channel];
    if (!state->active || state->bytes_remaining == 0) return 0;

    /* If header not yet read, prepare the full decompression context.
     * We decompress the entire block into a temporary buffer, then
     * feed bytes out one at a time. This is simpler than maintaining
     * a streaming context across calls, and S-DD1 blocks are small
     * (typically < 64KB). */
    if (!state->header_read) {
        /* We need to know how many bytes to decompress.
         * bytes_remaining is the DMA transfer size, but the
         * decompressed data may be larger. We decompress up to
         * bytes_remaining bytes (the DMA will stop collecting). */
        uint32_t max_out = state->bytes_remaining;
        if (max_out > 0x10000) max_out = 0x10000;

        /* Decompress the entire block using the unified bsnes engine.
         * Uses MMC reads at byte_ptr via stream_sdd1. */
        Sdd1Decomp d;
        memset(&d, 0, sizeof(d));
        d.read_byte = sdd1_read_from_stream;
        d.read_ctx = &d;
        d.stream_sdd1 = sdd1;
        d.byte_ptr = state->src_pos;
        d.length = (uint16_t)max_out;

        sdd1_prepareDecomp(&d);

        /* Decompress into temporary buffer */
        uint8_t *tmp = (uint8_t *)malloc(max_out);
        if (!tmp) {
            state->active = false;
            sdd1->r4801 &= (uint8_t)~(1u << channel);
            return 0;
        }
        memset(tmp, 0, max_out);
        sdd1_ol_launch(&d, tmp);

        /* Store decompressed data in the DMA state for byte-by-byte delivery.
         * We use the context_states array to store the buffer pointer (ugly but
         * avoids struct changes). Instead, we allocate a per-channel buffer. */
        if (state->decomp_buf) free(state->decomp_buf);
        state->decomp_buf = tmp;
        state->decomp_buf_size = max_out;
        state->decomp_pos = 0;
        state->header_read = true;

        /* Update src_pos to where the compressed data ended */
        state->src_pos = d.byte_ptr;

        /* Diagnostic: log decompressed output for first few sessions */
        if (sdd1_debug_enabled()) {
          static int decomp_log = 0;
          if (decomp_log < 10) {
            fprintf(stderr, "[SDD1_DECOMP] ch%d src=%06X size=%u decomp=%u first16: ",
                    channel, state->src_addr, (unsigned)state->bytes_remaining,
                    (unsigned)max_out);
            for (uint32_t k = 0; k < 16 && k < max_out; k++)
              fprintf(stderr, "%02X ", tmp[k]);
            fprintf(stderr, "\n");
            decomp_log++;
          }
        }
    }

    /* Return next byte from decompressed buffer */
    if (state->decomp_pos < state->decomp_buf_size) {
        uint8_t val = state->decomp_buf[state->decomp_pos++];
        state->bytes_remaining--;
        if (state->bytes_remaining == 0) {
            state->active = false;
            sdd1->r4801 &= (uint8_t)~(1u << channel);
        }
        return val;
    }

    /* Buffer exhausted but bytes_remaining > 0: return 0 */
    state->bytes_remaining--;
    if (state->bytes_remaining == 0) {
        state->active = false;
        sdd1->r4801 &= (uint8_t)~(1u << channel);
    }
    return 0;
}

/* ---- Helper functions ---- */

bool sdd1_dma_active(Sdd1* sdd1, int channel) {
    if (!sdd1 || channel < 0 || channel > 7) return false;
    return sdd1->dma_state[channel].active && sdd1->dma_state[channel].bytes_remaining > 0;
}

uint32_t sdd1_dma_src_addr(Sdd1* sdd1, int channel) {
    if (!sdd1 || channel < 0 || channel > 7) return 0;
    return sdd1->dma_state[channel].src_addr;
}

uint8_t sdd1_mmc_read(Sdd1* sdd1, uint32_t addr24) {
    if (!sdd1) return 0;
    return sdd1_mmc_read_byte(sdd1, addr24);
}

uint32_t sdd1_mmc_offset(Sdd1* sdd1, uint32_t addr24) {
    if (!sdd1 || !sdd1->rom || sdd1->rom_size == 0) return UINT32_MAX;
    uint32_t off = sdd1_mmc_linear(sdd1, addr24);
    /* Dev: dump MMC bank->linear offset map for the frame regions of interest
     * (text C2:DBxx / title CA:6Dxx / intro C2:0Bxx read via $C2-ish logical
     * PCs in banks C0-FF). Zero host cost unless SNESRECOMP_MMC_MAP=1. */
    {
        static int _mmc_map = -1;
        if (_mmc_map < 0) {
            const char *_e = getenv("SNESRECOMP_MMC_MAP");
            _mmc_map = (_e && _e[0] && _e[0] != '0') ? 1 : 0;
        }
        if (_mmc_map) {
            uint16_t a16 = (uint16_t)(addr24 & 0xFFFFu);
            if (((addr24 >> 16) == 0xDB && (a16 >= 0x00 && 0)) ||
                ((addr24 & 0xFFF000u) == 0xC2D000u) ||
                ((addr24 & 0xFFF000u) == 0xCA6000u) ||
                ((addr24 & 0xFFF000u) == 0xC20000u)) {
                fprintf(stderr,
                        "[MMC_MAP] addr24=%06X off=%06X p4=%02X p5=%02X p6=%02X p7=%02X\n",
                        (unsigned)addr24, (unsigned)off,
                        sdd1->r4804, sdd1->r4805, sdd1->r4806, sdd1->r4807);
            }
        }
    }
    return off < sdd1->rom_size ? off : UINT32_MAX;
}

uint32_t sdd1_lorom_window_offset(Sdd1* sdd1, uint8_t bank, uint16_t adr) {
    if (!sdd1 || !sdd1->rom || sdd1->rom_size == 0) return UINT32_MAX;
    if (adr < 0x8000) return UINT32_MAX;

    uint8_t canonical = bank & 0x7f;
    if (canonical >= 0x40) return UINT32_MAX;

    if (canonical >= 0x20) {
        if (bank < 0x80) {
            if (sdd1->r4805 & 0x80) canonical &= 0x1f;
        } else {
            if (sdd1->r4807 & 0x80) canonical &= 0x1f;
        }
    }

    uint32_t off = ((uint32_t)canonical << 15) | (adr & 0x7fff);
    return off < sdd1->rom_size ? off : UINT32_MAX;
}

/* ---- CPU-read decompression (bsnes SDD1::read emulation) ---- */

/* Called by snes_write when the CPU writes to $43x0-$43x7 (DMA channel regs).
 * The real S-DD1 spies on these writes to capture source address/size so it
 * knows what address to match against for CPU-read decompression. */
void sdd1_dma_channel_write(Sdd1 *sdd1, uint16_t addr, uint8_t val) {
    if (!sdd1) return;
    if (addr < 0x4300 || addr > 0x437f) return;
    unsigned ch = (addr >> 4) & 7;
    unsigned reg = addr & 0xf;
    /* Diagnostic: log first 30 channel register writes */
    if (sdd1_debug_enabled()) {
      static int ch_log = 0;
      if (ch_log < 30 && reg >= 2 && reg <= 6) {
        fprintf(stderr, "[SDD1_CH_WRITE] ch%d $%04X=$%02X\n", ch, addr, val);
        ch_log++;
      }
    }
    switch (reg) {
        case 2: sdd1->cpu_ch[ch].addr = (sdd1->cpu_ch[ch].addr & 0xffff00u) | ((uint32_t)val <<  0); break;
        case 3: sdd1->cpu_ch[ch].addr = (sdd1->cpu_ch[ch].addr & 0xff00ffu) | ((uint32_t)val <<  8); break;
        case 4: sdd1->cpu_ch[ch].addr = (sdd1->cpu_ch[ch].addr & 0x00ffffu) | ((uint32_t)val << 16); break;
        case 5: sdd1->cpu_ch[ch].size  = (sdd1->cpu_ch[ch].size  &  0xff00u) | (val << 0); break;
        case 6: sdd1->cpu_ch[ch].size  = (sdd1->cpu_ch[ch].size  &  0x00ffu) | (val << 8); break;
    }
}

/* Called by cart_read when the CPU reads from $C0-$FF:$8000-$FFFF.
 * Returns true and fills *data if decompression is active for this address.
 * Returns false if the read should fall through to raw ROM. */
bool sdd1_cpu_read(Sdd1 *sdd1, uint32_t addr24, uint8_t *data) {
    if (!sdd1 || !data) return false;
    uint8_t en = sdd1->r4800 & sdd1->r4801;
    /* Diagnostic: log first 30 calls to see what addresses arrive */
    if (sdd1_debug_enabled()) {
      static int cpu_read_log = 0;
      if (cpu_read_log < 20) {
        fprintf(stderr, "[SDD1_CPU_READ] addr=%06X en=$%02X r4800=$%02X r4801=$%02X\n",
                addr24, en, sdd1->r4800, sdd1->r4801);
        cpu_read_log++;
      }
    }
    if (!en) return false;

    for (int i = 0; i < 8; i++) {
        if (!(en & (1 << i))) continue;
        if (sdd1->cpu_ch[i].addr != addr24) continue;

        /* Address match — decompress on first access */
        if (!sdd1->cpu_ch[i].buf) {
            uint32_t sz = sdd1->cpu_ch[i].size ? sdd1->cpu_ch[i].size : 0x10000;
            if (sz > 0x10000) sz = 0x10000;

            uint8_t *tmp = (uint8_t *)malloc(sz);
            if (!tmp) return false;
            memset(tmp, 0, sz);

            /* Decompress using the same engine as sdd1_dma_init */
            {
                Sdd1Decomp d;
                memset(&d, 0, sizeof(d));
                d.read_byte = sdd1_read_from_stream;
                d.read_ctx = &d;
                d.stream_sdd1 = sdd1;
                d.byte_ptr = addr24;
                d.length = (uint16_t)sz;

                sdd1_prepareDecomp(&d);
                sdd1_ol_launch(&d, tmp);
            }

            sdd1->cpu_ch[i].buf = tmp;
            sdd1->cpu_ch[i].buf_size = sz;
            sdd1->cpu_ch[i].buf_pos = 0;

            if (sdd1_debug_enabled()) {
              static int cpu_decomp_log = 0;
              if (cpu_decomp_log < 10) {
                fprintf(stderr, "[SDD1_CPU_DECOMP] ch%d addr=%06X size=%u first16: ",
                        i, addr24, (unsigned)sz);
                for (uint32_t k = 0; k < 16 && k < sz; k++)
                  fprintf(stderr, "%02X ", tmp[k]);
                fprintf(stderr, "\n");
                cpu_decomp_log++;
              }
            }
        }

        /* Return next decompressed byte */
        if (sdd1->cpu_ch[i].buf_pos < sdd1->cpu_ch[i].buf_size) {
            *data = sdd1->cpu_ch[i].buf[sdd1->cpu_ch[i].buf_pos++];
            if (sdd1->cpu_ch[i].buf_pos >= sdd1->cpu_ch[i].buf_size) {
                /* Transfer complete — free buffer, disarm channel */
                free(sdd1->cpu_ch[i].buf);
                sdd1->cpu_ch[i].buf = NULL;
                sdd1->cpu_ch[i].buf_size = 0;
                sdd1->cpu_ch[i].buf_pos = 0;
                sdd1->r4801 &= ~(1 << i);
                sdd1->cpu_ch[i].armed = false;
            }
            return true;
        }
    }
    return false;
}

/* Directly decompress S-DD1 compressed data from ROM into a buffer.
 * Used as a workaround when the game's normal Mode 0 setup code
 * (at $CA:643E-$CA:64A9) is not reached by the recompiler's LLE.
 * Returns the number of bytes decompressed. */
uint32_t sdd1_decompress_to_buf(Sdd1 *sdd1, uint32_t src_addr24,
                                uint32_t size, uint8_t *out_buf,
                                uint32_t out_buf_size) {
    if (!sdd1 || !out_buf || size == 0 || out_buf_size == 0) return 0;
    if (!sdd1->rom || sdd1->rom_size == 0) return 0;

    uint32_t actual_size = size;
    if (actual_size > out_buf_size) actual_size = out_buf_size;

    /* Set up decompression context using the same engine as sdd1_dma_init */
    Sdd1Decomp d;
    memset(&d, 0, sizeof(d));
    d.read_byte = sdd1_read_from_stream;
    d.read_ctx = &d;
    d.stream_sdd1 = sdd1;
    d.byte_ptr = src_addr24;
    d.length = (uint16_t)(actual_size > 0xFFFF ? 0xFFFF : actual_size);

    sdd1_prepareDecomp(&d);
    sdd1_ol_launch(&d, out_buf);
    return actual_size;
}

/* Test function: decompress known data and verify output */
uint32_t sdd1_test_decompress(Sdd1 *sdd1, uint32_t src_addr24,
                                uint32_t size, uint8_t *out_buf,
                                uint32_t out_buf_size) {
    if (!sdd1 || !out_buf || size == 0 || out_buf_size == 0) return 0;
    if (!sdd1->rom || sdd1->rom_size == 0) return 0;

    uint32_t actual_size = size;
    if (actual_size > out_buf_size) actual_size = out_buf_size;

    /* Use a flat buffer copy instead of MMC translation */
    uint32_t rom_offset = sdd1_mmc_offset(sdd1, src_addr24);
    if (rom_offset == UINT32_MAX) return 0;
    
    /* Copy compressed data to a flat buffer */
    uint8_t *flat_data = (uint8_t *)malloc(actual_size + 256);
    if (!flat_data) return 0;
    for (uint32_t i = 0; i < actual_size + 256; i++) {
        uint32_t off = rom_offset + i;
        flat_data[i] = (off < sdd1->rom_size) ? sdd1->rom[off] : 0;
    }

    Sdd1Decomp d;
    memset(&d, 0, sizeof(d));
    d.read_byte = sdd1_read_from_buf;
    d.read_ctx = &d;
    d.rom_ptr = flat_data;
    d.byte_ptr = 0;
    d.length = (uint16_t)(actual_size > 0xFFFF ? 0xFFFF : actual_size);

    sdd1_prepareDecomp(&d);
    sdd1_ol_launch(&d, out_buf);

    free(flat_data);
    return actual_size;
}
