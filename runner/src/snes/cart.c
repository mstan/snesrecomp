#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../types.h"
#include "cart.h"
#include "snes.h"
#include "superfx.h"
#include "cx4.h"
#include "dsp1.h"
#include "sa1.h"
#include "sdd1.h"

extern uint32_t g_interp816_cur_pc;

static uint8_t cart_readLorom(Cart* cart, uint8_t bank, uint16_t adr);
static void cart_writeLorom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);
static uint8_t cart_readHirom(Cart* cart, uint8_t bank, uint16_t adr);
static void cart_writeHirom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);

Cart* cart_init(Snes* snes) {
  Cart* cart = calloc(1, sizeof(Cart));  /* zero padding: saveload/co-sim hash determinism */
  cart->snes = snes;
  cart->type = 0;
  cart->rom = NULL;
  cart->romSize = 0;
  cart->ram = NULL;
  cart->ramSize = 0;
  cart->masterClock = &snes->beamMasterLast;
  return cart;
}

void cart_free(Cart* cart) {
  superfx_destroy(cart->superfx);
  cx4_destroy(cart->cx4);
  dsp1_destroy(cart->dsp1);
  sa1_destroy(cart->sa1);
  sdd1_destroy(cart->sdd1);
  free(cart->rom);
  free(cart->ram);
  free(cart);
}

void cart_reset(Cart* cart) {
  //if(cart->ramSize > 0 && cart->ram != NULL) memset(cart->ram, 0, cart->ramSize); // for now
  if (cart->superfx) superfx_reset(cart->superfx);
  if (cart->cx4) cx4_reset(cart->cx4);
  if (cart->dsp1) dsp1_reset(cart->dsp1);
  if (cart->sa1) sa1_reset(cart->sa1);
  if (cart->sdd1) sdd1_reset(cart->sdd1);
}

void cart_saveload(Cart *cart, SaveLoadInfo *sli) {
  sli->func(sli, cart->ram, cart->ramSize);
  /* Cx4 games have no battery RAM, so the block above streams nothing; the
   * coprocessor's own 8 KB of working RAM is the guest-visible state that a
   * mid-game state must carry. */
  if (cart->cx4) cx4_saveload(cart->cx4, sli);
  if (cart->dsp1) dsp1_saveload(cart->dsp1, sli);
  if (cart->sdd1) sdd1_saveload(cart->sdd1, sli);
  if (cart->sa1) {
    sli->func(sli, &cart->cpuBusAddress, sizeof(cart->cpuBusAddress));
    sa1_saveload(cart->sa1, sli);
  }
}

void cart_load(Cart* cart, int type, uint8_t* rom, int romSize, int ramSize) {
  superfx_destroy(cart->superfx);
  cart->superfx = NULL;
  cx4_destroy(cart->cx4);
  cart->cx4 = NULL;
  dsp1_destroy(cart->dsp1);
  cart->dsp1 = NULL;
  sa1_destroy(cart->sa1);
  cart->sa1 = NULL;
  sdd1_destroy(cart->sdd1);
  cart->sdd1 = NULL;
  cart->type = type;
  if(cart->rom != NULL) free(cart->rom);
  if(cart->ram != NULL) free(cart->ram);
  cart->rom = malloc(romSize);
  cart->romSize = romSize;
  if(ramSize > 0) {
    cart->ram = malloc(ramSize);
    memset(cart->ram, 0, ramSize);
  } else {
    cart->ram = NULL;
  }
  cart->ramSize = ramSize;
  memcpy(cart->rom, rom, romSize);
  if (type == CART_SUPERFX)
    cart->superfx = superfx_create(cart->rom, cart->romSize,
                                   cart->ram, cart->ramSize);
  if (type == CART_CX4) {
    cart->cx4 = cx4_create(cart->rom, cart->romSize, cart->ram, cart->ramSize);
    /* The HG51B S169's internal reciprocal table is not in the game ROM. This
     * reports loudly on failure rather than computing on zeros. */
    (void)cx4_load_firmware(cart->cx4, NULL);
  }
  if (type == CART_DSP1 || type == CART_DSP1_HIROM) {
    cart->dsp1 = dsp1_create();
    (void)dsp1_load_firmware(cart->dsp1, NULL);
  }
  if (type == CART_SDD1)
    cart->sdd1 = sdd1_create(cart->rom, cart->romSize, cart->ram, cart->ramSize);
  if (type == CART_SA1)
    cart->sa1 = sa1_create(cart->rom, cart->romSize,
                           cart->ram, cart->ramSize);
}

void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
  if (cart && cart->superfx) superfx_sync(cart->superfx, master_clock);
  /* Instruction-level Cx4: its results appear over time, so it must be caught
   * up to the CPU's clock before anything observes its state. */
  if (cart && cart->cx4) cx4_sync(cart->cx4, master_clock);
  if (cart && cart->dsp1) dsp1_sync(cart->dsp1, master_clock);
  if (cart && cart->sdd1) sdd1_sync(cart->sdd1, master_clock);
  if (cart && cart->sa1) {
    sa1_set_cpu_bus_address(cart->sa1, cart->cpuBusAddress);
    sa1_sync(cart->sa1, master_clock);
  }
}

void cart_set_master_clock_source(Cart *cart, const uint64_t *master_clock) {
  if (cart) cart->masterClock = master_clock;
}

void cart_note_cpu_bus(Cart *cart, uint8_t bank, uint16_t address) {
  if (cart)
    cart->cpuBusAddress = ((uint32_t)bank << 16) | address;
}

static uint64_t cart_master_clock(const Cart *cart) {
  return cart && cart->masterClock ? *cart->masterClock : 0;
}

uint8_t *cart_getRomPtr(Cart *cart, uint8_t bank, uint16_t adr) {
  if (!cart || !cart->rom || cart->romSize == 0) return NULL;
  if (bank == 0x7e || bank == 0x7f) return NULL;
  /* Resolve the linear offset for the caller (SDD1 MMC path computes its own) */
  uint32_t off;
  switch (cart->type) {
    case CART_LOROM: {
      if ((((bank >= 0x70 && bank < 0x7e) || bank >= 0xf0)) &&
          adr < 0x8000 && cart->ramSize > 0) return NULL;
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)canonical << 15) | (adr & 0x7fff);
      break;
    }
    case CART_DSP1: {
      if (cart_is_dsp1_window(cart, bank, adr) ||
          (cart->ramSize > 0 && cart_is_dsp1_sram_window(cart, bank, adr)))
        return NULL;
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)canonical << 15) | (adr & 0x7fff);
      break;
    }
    case CART_DSP1_HIROM: {
      if (cart_is_dsp1_window(cart, bank, adr) ||
          (cart->ramSize > 0 && cart_is_dsp1_sram_window(cart, bank, adr)))
        return NULL;
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)(canonical & 0x3f) << 16) | adr;
      break;
    }
    case CART_HIROM: {
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)(canonical & 0x3f) << 16) | adr;
      break;
    }
case CART_CX4: {
      /* Cx4 carts are LoROM. The $6000-$7FFF window in banks $00-$3F/$80-$BF
       * belongs to the coprocessor, not the ROM — return NULL so callers route
       * it through cart_read/cart_write. */
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)canonical << 15) | (adr & 0x7fff);
      break;
    }
    case CART_SDD1: {
      /* S-DD1 carts are LoROM. The $4800-$4807 window in banks $00-$3F/$80-$BF
       * belongs to the decompression chip, not the ROM — return NULL so callers
       * route it through cart_read/cart_write. */
      if (bank >= 0xC0 && bank <= 0xFF) {
        /* MMC banking for banks C0-FF via $4804-$4807. Resolve to the linear
         * ROM byte backing this address and return a pointer to it (RomPtr and
         * cart_readLorom dereference the result); NULL when disabled. */
        if (cart->sdd1) {
          uint32_t off = sdd1_mmc_offset(cart->sdd1, ((uint32_t)bank << 16) | adr);
          if (off != UINT32_MAX) {
#if defined(SNESRECOMP_TRACE) && SNESRECOMP_TRACE
            static int gpr_trace_on = -1;
            if (gpr_trace_on < 0) gpr_trace_on = getenv("SNESRECOMP_ROM_PTR_TRACE") ? 1 : 0;
            if (gpr_trace_on && off >= 0x5E0000 && off < 0x600000) {
              static unsigned long n;
              if (n++ < 2000000)
                fprintf(stderr, "[romptr] pc=%06X bank=%02X adr=%04X off=%06X val=%02X\n",
                        (unsigned)g_interp816_cur_pc, bank, adr, off, cart->rom[off % cart->romSize]);
            }
#endif
            return &cart->rom[off % cart->romSize];
          }
        }
        return NULL;
      }
      /* Banks $00-$3F/$80-$BF: standard LoROM mapping with the S-DD1 bit-7
       * MMC override from bsnes's mcuRead (banks 20-3F alias the first 1MB
       * when r4805 & 0x80, banks A0-BF when r4807 & 0x80). */
      if (cart->sdd1) {
        uint32_t off2 = sdd1_lorom_window_offset(cart->sdd1, bank, adr);
        if (off2 != UINT32_MAX)
          return &cart->rom[off2 % cart->romSize];
      }
      /* Legacy LoROM mapping for the remaining banks (40-7F, or no chip) */
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)canonical << 15) | (adr & 0x7fff);
      break;
    }
    case CART_SA1:
      return sa1_cpu_memory_ptr(cart->sa1, bank, adr);
    default:
      return NULL;
  }
  return &cart->rom[off % cart->romSize];
}

uint8_t cart_read(Cart* cart, uint8_t bank, uint16_t adr) {
  switch(cart->type) {
    case 0:
      assert(0);
      return 0;
    case CART_LOROM: return cart_readLorom(cart, bank, adr);
    case CART_HIROM: return cart_readHirom(cart, bank, adr);
    case CART_DSP1:
    case CART_DSP1_HIROM:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      if (cart_is_dsp1_window(cart, bank, adr))
        return dsp1_read(cart->dsp1, cart_dsp1_register(adr));
      if (cart->ramSize > 0 && cart_is_dsp1_sram_window(cart, bank, adr))
        return cart->ram[(adr & 0x1fff) & (cart->ramSize - 1)];
      return cart->type == CART_DSP1_HIROM
          ? cart_readHirom(cart, bank, adr)
          : cart_readLorom(cart, bank, adr);
case CART_CX4:
      /* Catch the DSP up before observing it: unlike a command-level model,
       * an instruction-level Cx4 produces results as its clock advances. */
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      if (cart_is_cx4_window(cart, bank, adr))
        return cx4_read(cart->cx4, adr);
      /* While the DSP owns the bus, a CPU read of the vector area returns Cx4
       * IO instead of ROM — that is how the coprocessor overrides the vectors. */
      if (cx4_owns_bus(cart->cx4) && (bank & 0x7f) == 0x00 &&
          adr >= 0xffc0)
        return cx4_read_vector_override(cart->cx4, adr);
      return cart_readLorom(cart, bank, adr);
    case CART_SDD1:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      if (cart_is_sdd1_window(cart, bank, adr))
        return sdd1_read(cart->sdd1, adr);
      /* S-DD1 CPU-read decompression: when the CPU reads from $C0-$FF:$8000-$FFFF,
       * check if an active S-DD1 channel matches this address and return
       * decompressed data (bsnes SDD1::read emulation). */
      if (bank >= 0xc0 && adr >= 0x8000 && cart->sdd1) {
        uint8_t cpu_data;
        uint32_t addr24 = ((uint32_t)bank << 16) | adr;
        if (sdd1_cpu_read(cart->sdd1, addr24, &cpu_data))
          return cpu_data;
      }
      /* S-DD1 MMC window: every read from banks $C0-$FF goes through the
       * MMC pages ($4804-$4807) exactly like bsnes SDD1::read, regardless of
       * whether decompression is active. The plain LoROM mapping is WRONG here
       * for addresses below $8000 (e.g. bridge palette DMA source FD:5419
       * resolves to MMC offset $3D5419, not LoROM $1ED419) — using it corrupts
       * CGRAM palettes loaded from these banks. */
      if (bank >= 0xc0 && cart->sdd1)
        return sdd1_mmc_read(cart->sdd1, ((uint32_t)bank << 16) | adr);
      return cart_readLorom(cart, bank, adr);
    case CART_SUPERFX:
      if ((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) &&
          adr >= 0x3000 && adr <= 0x32ff)
        return superfx_cpu_read_io(cart->superfx, adr);
      if ((bank == 0x70 || bank == 0x71 || bank == 0xf0 || bank == 0xf1))
        return superfx_cpu_read_ram(cart->superfx,
                                    ((uint32_t)(bank & 1) << 16) | adr, 0);
      /* CPU-visible ROM uses the GSU LoROM/linear mappings and observes the
       * vector override while the coprocessor owns ROM. */
      if (adr >= 0x8000 || (bank & 0x7f) >= 0x40) {
        uint8_t b = bank & 0x7f;
        uint32_t off = b < 0x40 ? ((uint32_t)b << 15) | (adr & 0x7fff)
                                : ((uint32_t)(b - 0x40) << 16) | adr;
        return superfx_cpu_read_rom(cart->superfx, off, 0);
      }
      return 0;
    case CART_SA1:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      return sa1_cpu_read(cart->sa1, bank, adr, 0xff);
  }
  assert(0);
  return 0;
}

void cart_write(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  switch(cart->type) {
    case 0: break;
    case CART_LOROM: cart_writeLorom(cart, bank, adr, val); break;
    case CART_HIROM: cart_writeHirom(cart, bank, adr, val); break;
    case CART_DSP1:
    case CART_DSP1_HIROM:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      if (cart_is_dsp1_window(cart, bank, adr))
        dsp1_write(cart->dsp1, cart_dsp1_register(adr), val);
      else if (cart->ramSize > 0 && cart_is_dsp1_sram_window(cart, bank, adr))
        cart->ram[(adr & 0x1fff) & (cart->ramSize - 1)] = val;
      else if (cart->type == CART_DSP1_HIROM)
        cart_writeHirom(cart, bank, adr, val);
      else
        cart_writeLorom(cart, bank, adr, val);
      break;
case CART_CX4:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      if (cart_is_cx4_window(cart, bank, adr))
        cx4_write(cart->cx4, adr, val);
      else
        cart_writeLorom(cart, bank, adr, val);
      break;
    case CART_SDD1:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      if (cart_is_sdd1_window(cart, bank, adr))
        sdd1_write(cart->sdd1, adr, val);
      else
        cart_writeLorom(cart, bank, adr, val);
      break;
    case CART_SUPERFX:
      if ((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) &&
          adr >= 0x3000 && adr <= 0x32ff)
        superfx_cpu_write_io(cart->superfx, adr, val);
      else if (bank == 0x70 || bank == 0x71 || bank == 0xf0 || bank == 0xf1)
        superfx_cpu_write_ram(cart->superfx,
                              ((uint32_t)(bank & 1) << 16) | adr, val);
      break;
    case CART_SA1:
      cart_sync_coprocessors(cart, cart_master_clock(cart));
      sa1_cpu_write(cart->sa1, bank, adr, val);
      break;
  }
}

#include "../cpu_trace.h"

static uint8_t cart_readLorom(Cart* cart, uint8_t bank, uint16_t adr) {
  if(((bank >= 0x70 && bank < 0x7e) || bank >= 0xf0) && adr < 0x8000 && cart->ramSize > 0) {
    // banks 70-7e and f0-ff, adr 0000-7fff
    return cart->ram[(((bank & 0xf) << 15) | adr) & (cart->ramSize - 1)];
  }
  uint8_t *rom = cart_getRomPtr(cart, bank, adr);
  if (rom) {
    return *rom;
  }
  /* Out-of-range cart read. No printf — the ring buffer is the
   * channel. cpu_trace_offrails dumps trace at hit#1 + every 64th
   * so we see the chain WITHOUT million-line stderr floods. */
  cpu_trace_offrails("cart_readLorom", (uint32_t)bank << 16 | adr);
  return 0;
}

static void cart_writeLorom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  if(((bank >= 0x70 && bank < 0x7e) || bank > 0xf0) && adr < 0x8000 && cart->ramSize > 0) {
    // banks 70-7e and f0-ff, adr 0000-7fff
    cart->ram[(((bank & 0xf) << 15) | adr) & (cart->ramSize - 1)] = val;
  }
}

static uint8_t cart_readHirom(Cart* cart, uint8_t bank, uint16_t adr) {
  uint8_t canonical = bank & 0x7f;
  if(canonical < 0x40 && adr >= 0x6000 && adr < 0x8000 && cart->ramSize > 0) {
    // banks 00-3f and 80-bf, adr 6000-7fff
    return cart->ram[(((canonical & 0x3f) << 13) | (adr & 0x1fff)) & (cart->ramSize - 1)];
  }
  uint8_t *rom = cart_getRomPtr(cart, bank, adr);
  if (rom) return *rom;
  assert(0);
  return 0;
}

static void cart_writeHirom(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  bank &= 0x7f;
  if(bank < 0x40 && adr >= 0x6000 && adr < 0x8000 && cart->ramSize > 0) {
    // banks 00-3f and 80-bf, adr 6000-7fff
    cart->ram[(((bank & 0x3f) << 13) | (adr & 0x1fff)) & (cart->ramSize - 1)] = val;
  }
}
