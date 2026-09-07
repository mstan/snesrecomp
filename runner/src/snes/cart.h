#ifndef CART_H
#define CART_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Cart Cart;
typedef struct SuperFx SuperFx;
typedef struct Cx4 Cx4;
typedef struct Dsp1 Dsp1;
typedef struct Sa1 Sa1;
typedef struct Sdd1 Sdd1;

#include "snes.h"

struct Cart {
  Snes* snes;
  uint8_t type;

  uint8_t* rom;
  uint32_t romSize;
  uint8_t* ram;
  uint32_t ramSize;
  const uint64_t* masterClock;
  uint32_t cpuBusAddress;
  SuperFx* superfx;
  Cx4* cx4;
  Dsp1* dsp1;
  Sa1* sa1;
  Sdd1* sdd1;
};

#define CART_LOROM        1
#define CART_HIROM        2
#define CART_SUPERFX      3
#define CART_CX4          4
#define CART_DSP1         5
#define CART_DSP1_HIROM   6
#define CART_SA1          7
#define CART_SDD1 8

static inline bool cart_has_sa1(const Cart *cart) {
  return cart && cart->type == CART_SA1 && cart->sa1;
}

static inline bool cart_has_sdd1(const Cart *cart) {
  return cart && cart->type == CART_SDD1 && cart->sdd1;
}

static inline bool cart_has_dsp1(const Cart* cart) {
  return cart &&
         (cart->type == CART_DSP1 || cart->type == CART_DSP1_HIROM);
}

/* True when (bank, adr) lands in the Capcom Cx4's CPU-visible window:
 * banks $00-$3F / $80-$BF, address $6000-$7FFF. Callers on the fast bus
 * paths (cpu_state.c) use this to route to cart_read/cart_write instead of
 * falling through to a ROM pointer. */
static inline bool cart_is_cx4_window(const Cart* cart, uint8_t bank,
                                      uint16_t adr) {
  return cart && cart->type == CART_CX4 && adr >= 0x6000 && adr < 0x8000 &&
         (bank < 0x40 || (bank >= 0x80 && bank < 0xc0));
}

/* Super Mario Kart's SHVC-1K1X DSP-1 host port:
 * banks $00-$1F / $80-$9F, addresses $6000-$7FFF, mirrored by mask $0FFF.
 * The mapper removes the low 12 address bits: $6000-$6FFF selects DR and
 * $7000-$7FFF selects SR. */
static inline bool cart_is_dsp1_window(const Cart* cart, uint8_t bank,
                                       uint16_t adr) {
  return cart_has_dsp1(cart) && adr >= 0x6000 && adr < 0x8000 &&
         (bank < 0x20 || (bank >= 0x80 && bank < 0xa0));
}

static inline uint16_t cart_dsp1_register(uint16_t adr) {
  return (uint16_t)((adr >> 12) & 1u);
}

/* SHVC-1K1X battery SRAM is separate from the usual LoROM $70-$7D window. */
static inline bool cart_is_dsp1_sram_window(const Cart* cart, uint8_t bank,
                                            uint16_t adr) {
  return cart_has_dsp1(cart) && adr >= 0x6000 && adr < 0x8000 &&
         ((bank >= 0x20 && bank < 0x40) ||
          (bank >= 0xa0 && bank < 0xc0));
}

/* S-DD1 decompression chip registers:
 * banks $00-$3F / $80-$BF, addresses $4800-$4807. */
static inline bool cart_is_sdd1_window(const Cart* cart, uint8_t bank,
                                       uint16_t adr) {
  return cart && cart->type == CART_SDD1 && adr >= 0x4800 && adr < 0x4808 &&
         (bank < 0x40 || (bank >= 0x80 && bank < 0xc0));
}

void cart_sync_coprocessors(Cart *cart, uint64_t master_clock);
void cart_set_master_clock_source(Cart *cart, const uint64_t *master_clock);
void cart_note_cpu_bus(Cart *cart, uint8_t bank, uint16_t address);

// TODO: how to handle reset & load? (especially where to init ram)

Cart* cart_init(Snes* snes);
void cart_free(Cart* cart);
void cart_reset(Cart* cart); // will reset special chips etc, general reading is set up in load
void cart_load(Cart* cart, int type, uint8_t* rom, int romSize, int ramSize); // TODO: figure out how to handle (battery, cart-chips etc)
uint8_t cart_read(Cart* cart, uint8_t bank, uint16_t adr);
void cart_write(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);
void cart_saveload(Cart *cart, SaveLoadInfo *sli);
// Resolve a CPU-visible address to stable cartridge storage. Standard carts
// return ROM only; SA-1 may also return its I-RAM or BW-RAM backing. Returns
// NULL for I/O and switched-vector bytes.
uint8_t *cart_getRomPtr(Cart *cart, uint8_t bank, uint16_t adr);
#endif
