#ifndef SNES_SDD1_H
#define SNES_SDD1_H

#include <stdint.h>
#include <stdbool.h>
#include "saveload.h"

typedef struct Sdd1 Sdd1;

/* Per-channel state for streaming DMA decompression.
 * The chip pre-decompresses the entire block into a temporary buffer
 * on first byte request, then feeds bytes out one at a time. */
typedef struct Sdd1DmaState {
  bool active;
  uint32_t src_addr;        /* 24-bit MMC-window source captured at init */
  uint32_t src_pos;         /* Current MMC-window read position (24-bit) */
  uint32_t bytes_remaining; /* Bytes left to decompress (0x10000 default) */
  bool header_read;         /* Whether the block was pre-decompressed */
  uint8_t *decomp_buf;      /* Heap-allocated decompressed data */
  uint32_t decomp_buf_size; /* Size of decomp_buf */
  uint32_t decomp_pos;      /* Current read position in decomp_buf */
} Sdd1DmaState;

struct Sdd1 {
  /* S-DD1 register interface (bsnes semantics):
   * $4800 hard enable (DMA) / $4801 soft enable (decompression): bit n
   * selects S-CPU DMA channel n. $4804-$4807 are the MMC bank selects for
   * the C0-CF / D0-DF / E0-EF / F0-FF windows, each a 1MB ROM page. */
  uint8_t r4800;
  uint8_t r4801;
  uint8_t r4804;
  uint8_t r4805;
  uint8_t r4806;
  uint8_t r4807;

  const uint8_t *rom;
  uint32_t rom_size;

  /* Streaming DMA decompression state, one session per S-CPU DMA channel */
  Sdd1DmaState dma_state[8];

  /* CPU-read decompression: per-channel address/size captured from $43x0-$43x6
   * writes. When the CPU reads from $C0-$FF:$8000-$FFFF and the address matches
   * an active channel, we decompress and return the byte. */
  struct {
    uint32_t addr;  /* 24-bit source address from $43x2-$43x4 */
    uint16_t size;  /* 16-bit transfer size from $43x5-$43x6 */
    bool     armed; /* true after $4801 enables this channel */
    uint8_t *buf;   /* decompressed data (allocated on first match) */
    uint32_t buf_size;
    uint32_t buf_pos;
  } cpu_ch[8];
};

Sdd1 *sdd1_create(const uint8_t *rom, uint32_t rom_size, uint8_t *ram,
                  uint32_t ram_size);
void sdd1_destroy(Sdd1 *sdd1);
void sdd1_reset(Sdd1 *sdd1);
void sdd1_sync(Sdd1 *sdd1, uint64_t master_clock);
void sdd1_saveload(Sdd1 *sdd1, struct SaveLoadInfo *sli);
uint8_t sdd1_read(Sdd1 *sdd1, uint16_t addr);
void sdd1_write(Sdd1 *sdd1, uint16_t addr, uint8_t val);

/* Core decompression function (ported from bsnes-plus Andreas Naive) */
void sdd1_decompress(uint8_t *out, const uint8_t *in, int len);

/* Streaming decompression for DMA (byte-at-a-time). src_addr24 is the
 * S-CPU DMA source address in the C0-FF window; transfer_size is the DMA
 * transfer size (0 = 64KB). Requires r4800 & r4801 bits for the channel. */
void sdd1_dma_init(Sdd1 *sdd1, int channel, uint32_t src_addr24,
                   uint32_t transfer_size);
uint8_t sdd1_dma_get_byte(Sdd1 *sdd1, int channel);
bool sdd1_dma_active(Sdd1 *sdd1, int channel);
uint32_t sdd1_dma_src_addr(Sdd1 *sdd1, int channel);

/* Called by snes_write when the CPU writes to $43x0-$43x7 (DMA channel registers).
 * The S-DD1 spies on these writes to capture source address/size for CPU-read
 * decompression. Must be called for EVERY DMA register write, not just channel 0. */
void sdd1_dma_channel_write(Sdd1 *sdd1, uint16_t addr, uint8_t val);

/* Called by cart_read when the CPU reads from $C0-$FF:$8000-$FFFF.
 * Returns true and fills *data if decompression is active for this address.
 * Returns false if the read should fall through to raw ROM. */
bool sdd1_cpu_read(Sdd1 *sdd1, uint32_t addr24, uint8_t *data);

/* Directly decompress S-DD1 data and write to a byte buffer.
 * Used for Mode 0 tilemap setup when the game's normal code path
 * is not reached by the recompiler. Returns bytes written. */
uint32_t sdd1_decompress_to_buf(Sdd1 *sdd1, uint32_t src_addr24,
                                uint32_t size, uint8_t *out_buf,
                                uint32_t out_buf_size);

/* Test function: decompress from flat buffer (bypasses MMC) */
uint32_t sdd1_test_decompress(Sdd1 *sdd1, uint32_t src_addr24,
                              uint32_t size, uint8_t *out_buf,
                              uint32_t out_buf_size);

/* MMC banking for banks C0-FF ($4804-$4807) */
uint8_t sdd1_mmc_read(Sdd1 *sdd1, uint32_t addr24);
uint32_t sdd1_mmc_offset(Sdd1 *sdd1, uint32_t addr24);

/* Linear ROM offset for a LoROM-window read ($00-$3F/$80-$BF:8000-FFFF),
 * applying the S-DD1 bit-7 override from bsnes's mcuRead: banks 20-3F alias
 * the first 1MB of the window when r4805 & 0x80, and banks A0-BF do the same
 * when r4807 & 0x80. UINT32_MAX when the read is not LoROM-window ROM data
 * or lies beyond the cartridge size. */
uint32_t sdd1_lorom_window_offset(Sdd1 *sdd1, uint8_t bank, uint16_t adr);

#endif
