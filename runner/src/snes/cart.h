
#ifndef CART_H
#define CART_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct Cart Cart;
typedef struct SuperFx SuperFx;

#include "snes.h"

struct Cart {
  Snes* snes;
  uint8_t type;

  uint8_t* rom;
  uint32_t romSize;
  uint8_t* ram;
  uint32_t ramSize;
  SuperFx* superfx;
};

enum { CART_LOROM = 1, CART_HIROM = 2, CART_SUPERFX = 3 };

void cart_sync_coprocessors(Cart *cart, uint64_t master_clock);

// TODO: how to handle reset & load? (especially where to init ram)

Cart* cart_init(Snes* snes);
void cart_free(Cart* cart);
void cart_reset(Cart* cart); // will reset special chips etc, general reading is set up in load
void cart_load(Cart* cart, int type, uint8_t* rom, int romSize, int ramSize); // TODO: figure out how to handle (battery, cart-chips etc)
uint8_t cart_read(Cart* cart, uint8_t bank, uint16_t adr);
void cart_write(Cart* cart, uint8_t bank, uint16_t adr, uint8_t val);
void cart_saveload(Cart *cart, SaveLoadInfo *sli);
// Resolve a CPU-visible ROM address to stable cart storage. Returns NULL for
// WRAM, I/O, SRAM, or another non-ROM window.
uint8_t *cart_getRomPtr(Cart *cart, uint8_t bank, uint16_t adr);
#endif
