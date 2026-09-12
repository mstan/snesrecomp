#include "snes_state_digest.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/dsp.h"
#include "snes/joypad.h"
#include "snes/ppu.h"
#include "snes/saveload.h"

extern Snes *g_snes;

/*
 * Streaming CRC over a *_saveload byte stream.
 *
 * `skip_begin`/`skip_end` name a half-open window of ABSOLUTE stream offsets
 * to fold nothing for. Expressing the exclusion in stream offsets rather than
 * "ignore the Nth callback" keeps it correct no matter how the serializer
 * chops its writes — the DSP emits its whole region in one call today, and
 * this still holds if that is ever split.
 */
typedef struct CrcSli {
    SaveLoadInfo base;
    uint32_t     crc;
    size_t       pos;
    size_t       skip_begin;
    size_t       skip_end;
} CrcSli;

static void crc_sli_func(SaveLoadInfo *sli, void *data, size_t n)
{
    CrcSli *c = (CrcSli *)sli;
    const uint8_t *p = (const uint8_t *)data;
    size_t begin = c->pos;
    size_t end   = c->pos + n;

    c->pos = end;
    if (!p || n == 0)
        return;

    /* Fold [begin,end) minus [skip_begin,skip_end). */
    if (c->skip_end > c->skip_begin && begin < c->skip_end && end > c->skip_begin) {
        if (begin < c->skip_begin)
            c->crc = crc32_update(c->crc, p, c->skip_begin - begin);
        if (end > c->skip_end) {
            size_t tail_off = c->skip_end - begin;
            c->crc = crc32_update(c->crc, p + tail_off, end - c->skip_end);
        }
        return;
    }
    c->crc = crc32_update(c->crc, p, n);
}

static void crc_sli_init(CrcSli *c)
{
    memset(c, 0, sizeof(*c));
    c->base.func = &crc_sli_func;
    c->crc = CRC32_INIT;
}

/*
 * Byte offsets of the S-DSP output ring inside the stream `apu_saveload`
 * emits. See snes_state_digest.h — the ring's read cursor is advanced by the
 * SDL audio thread, so it is host presentation state and can never agree
 * between two peers.
 *
 * apu_saveload order: [Apu.ram .. Apu.pad+6) then dsp_saveload then
 * spc_saveload. dsp_saveload emits [Dsp.ram .. end of Dsp), and the ring
 * (sampleBuffer, sampleWrite, sampleRead) is that region's tail.
 */
#define APU_REGS_BYTES (offsetof(Apu, pad) + 6u - offsetof(Apu, ram))
#define DSP_REGION_BYTES (sizeof(Dsp) - offsetof(Dsp, ram))
#define DSP_SIM_PREFIX_BYTES (offsetof(Dsp, sampleBuffer) - offsetof(Dsp, ram))

size_t snes_digest_dsp_ring_bytes(void); /* also used by the snapshot restore */
size_t snes_digest_dsp_ring_bytes(void)
{
    return DSP_REGION_BYTES - DSP_SIM_PREFIX_BYTES;
}

static uint32_t digest_cpu(void)
{
    CrcSli c;
    crc_sli_init(&c);
    cpu_saveload(g_snes->cpu, &c.base);
    return crc32_final(c.crc);
}

/* WRAM plus the Snes tail the savestate carries (h/v counters, IRQ enables,
 * timers, auto-joypad, multiply/divide) and the joypad shift state. */
static uint32_t digest_wram(void)
{
    uint32_t crc = CRC32_INIT;
    crc = crc32_update(crc, (const uint8_t *)&g_snes->hPos,
                       sizeof(*g_snes) - offsetof(Snes, hPos));
    crc = crc32_update(crc, (const uint8_t *)g_snes->ram, 0x20000u);
    crc = crc32_update(crc, (const uint8_t *)&g_snes->ramAdr, sizeof(g_snes->ramAdr));
    crc = crc32_update(crc, (const uint8_t *)&g_snes->joypadStrobe,
                       sizeof(g_snes->joypadStrobe));
    crc = crc32_update(crc, (const uint8_t *)&g_snes->joypad1Index,
                       sizeof(g_snes->joypad1Index));
    crc = crc32_update(crc, (const uint8_t *)&g_snes->joypad2Index,
                       sizeof(g_snes->joypad2Index));
    crc = crc32_update(crc, (const uint8_t *)&g_snes->joypad1Latched,
                       sizeof(g_snes->joypad1Latched));
    crc = crc32_update(crc, (const uint8_t *)&g_snes->joypad2Latched,
                       sizeof(g_snes->joypad2Latched));
    /* Multitap seats, IOBit lines and per-bank shift counters. This is
     * guest-visible mid-frame state — a peer that rewound into the middle of
     * a five-player read has to resume in the same bank at the same bit —
     * and the same v8 savestate chunk carries it, so the digest domain stays
     * equal to the snapshot domain. */
    {
        size_t n = 0;
        const void *blob = joypad_state_blob(&n);
        if (blob && n)
            crc = crc32_update(crc, (const uint8_t *)blob, n);
    }
    return crc32_final(crc);
}

uint32_t snes_state_digest_apu_of(struct Apu *apu)
{
    CrcSli c;
    if (!apu)
        return 0u;
    crc_sli_init(&c);
    c.skip_begin = APU_REGS_BYTES + DSP_SIM_PREFIX_BYTES;
    c.skip_end   = APU_REGS_BYTES + DSP_REGION_BYTES;
    apu_saveload(apu, &c.base);
    return crc32_final(c.crc);
}

static uint32_t digest_apu(void)
{
    return snes_state_digest_apu_of(g_snes->apu);
}

static uint32_t digest_ppu(void)
{
    CrcSli c;
    crc_sli_init(&c);
    ppu_saveload(g_snes->ppu, &c.base);
    return crc32_final(c.crc);
}

static uint32_t digest_dma(void)
{
    CrcSli c;
    crc_sli_init(&c);
    dma_saveload(g_snes->dma, &c.base);
    return crc32_final(c.crc);
}

static uint32_t digest_cart(void)
{
    CrcSli c;
    crc_sli_init(&c);
    cart_saveload(g_snes->cart, &c.base);
    return crc32_final(c.crc);
}

void snes_state_digest_parts(SnesStateDigestParts *out)
{
    uint32_t master;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!g_snes)
        return;

    out->cpu  = digest_cpu();
    out->wram = digest_wram();
    out->apu  = digest_apu();
    out->ppu  = digest_ppu();
    out->dma  = digest_dma();
    out->cart = digest_cart();

    /* Master folds the partition CRCs in a fixed order, so it is stable
     * even if a partition is later re-partitioned internally. */
    master = CRC32_INIT;
    master = crc32_update(master, (const uint8_t *)&out->cpu, sizeof(out->cpu));
    master = crc32_update(master, (const uint8_t *)&out->wram, sizeof(out->wram));
    master = crc32_update(master, (const uint8_t *)&out->apu, sizeof(out->apu));
    master = crc32_update(master, (const uint8_t *)&out->ppu, sizeof(out->ppu));
    master = crc32_update(master, (const uint8_t *)&out->dma, sizeof(out->dma));
    master = crc32_update(master, (const uint8_t *)&out->cart, sizeof(out->cart));
    out->master = crc32_final(master);
}

uint32_t snes_state_digest(uint32_t partition)
{
    if (!g_snes)
        return 0u;
    switch (partition) {
    case SNES_DIGEST_PART_CPU:  return digest_cpu();
    case SNES_DIGEST_PART_WRAM: return digest_wram();
    case SNES_DIGEST_PART_APU:  return digest_apu();
    case SNES_DIGEST_PART_PPU:  return digest_ppu();
    case SNES_DIGEST_PART_DMA:  return digest_dma();
    case SNES_DIGEST_PART_CART: return digest_cart();
    case SNES_DIGEST_PART_MASTER: {
        SnesStateDigestParts p;
        snes_state_digest_parts(&p);
        return p.master;
    }
    default:
        return 0u;
    }
}

const char *snes_state_digest_part_name(uint32_t partition)
{
    switch (partition) {
    case SNES_DIGEST_PART_MASTER: return "master";
    case SNES_DIGEST_PART_CPU:    return "cpu";
    case SNES_DIGEST_PART_WRAM:   return "wram";
    case SNES_DIGEST_PART_APU:    return "apu";
    case SNES_DIGEST_PART_PPU:    return "ppu";
    case SNES_DIGEST_PART_DMA:    return "dma";
    case SNES_DIGEST_PART_CART:   return "cart";
    default:                      return "?";
    }
}

uint32_t snes_state_digest_first_diff(const SnesStateDigestParts *a,
                                      const SnesStateDigestParts *b)
{
    if (!a || !b)
        return SNES_DIGEST_PART_COUNT;
    if (a->cpu  != b->cpu)  return SNES_DIGEST_PART_CPU;
    if (a->wram != b->wram) return SNES_DIGEST_PART_WRAM;
    if (a->apu  != b->apu)  return SNES_DIGEST_PART_APU;
    if (a->ppu  != b->ppu)  return SNES_DIGEST_PART_PPU;
    if (a->dma  != b->dma)  return SNES_DIGEST_PART_DMA;
    if (a->cart != b->cart) return SNES_DIGEST_PART_CART;
    return SNES_DIGEST_PART_COUNT;
}
