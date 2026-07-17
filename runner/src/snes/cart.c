#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../types.h"
#include "cart.h"
#include "snes.h"
#include "superfx.h"

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
  return cart;
}

void cart_free(Cart* cart) {
  superfx_destroy(cart->superfx);
  free(cart->rom);
  free(cart->ram);
  free(cart);
}

void cart_reset(Cart* cart) {
  //if(cart->ramSize > 0 && cart->ram != NULL) memset(cart->ram, 0, cart->ramSize); // for now
  if (cart->superfx) superfx_reset(cart->superfx);
}

void cart_saveload(Cart *cart, SaveLoadInfo *sli) {
  sli->func(sli, cart->ram, cart->ramSize);
}

void cart_load(Cart* cart, int type, uint8_t* rom, int romSize, int ramSize) {
  superfx_destroy(cart->superfx);
  cart->superfx = NULL;
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
}

void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
  if (cart && cart->superfx) superfx_sync(cart->superfx, master_clock);
}

uint8_t *cart_getRomPtr(Cart *cart, uint8_t bank, uint16_t adr) {
  if (!cart || !cart->rom || cart->romSize == 0) return NULL;
  if (bank == 0x7e || bank == 0x7f) return NULL;
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
    case CART_HIROM: {
      uint8_t canonical = bank & 0x7f;
      if (adr < 0x8000 && canonical < 0x40) return NULL;
      off = ((uint32_t)(canonical & 0x3f) << 16) | adr;
      break;
    }
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
  }
  assert(0);
  return 0;
}

void cart_write(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val) {
  switch(cart->type) {
    case 0: break;
    case CART_LOROM: cart_writeLorom(cart, bank, adr, val); break;
    case CART_HIROM: cart_writeHirom(cart, bank, adr, val); break;
    case CART_SUPERFX:
      if ((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) &&
          adr >= 0x3000 && adr <= 0x32ff)
        superfx_cpu_write_io(cart->superfx, adr, val);
      else if (bank == 0x70 || bank == 0x71 || bank == 0xf0 || bank == 0xf1)
        superfx_cpu_write_ram(cart->superfx,
                              ((uint32_t)(bank & 1) << 16) | adr, val);
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
  if (rom) return *rom;
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
