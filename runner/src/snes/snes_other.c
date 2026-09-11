
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "snes.h"
#include "cart.h"
#include "ppu.h"
#include "dsp.h"
#if SNESRECOMP_ENABLE_MODS
#include "mod_runtime.h"
#endif

typedef struct CartHeader {
  // normal header
  uint8_t headerVersion; // 1, 2, 3
  char name[22]; // $ffc0-$ffd4 (max 21 bytes + \0), $ffd4=$00: header V2
  uint8_t speed; // $ffd5.7-4 (always 2 or 3)
  uint8_t type; // $ffd5.3-0
  uint8_t coprocessor; // $ffd6.7-4
  uint8_t chips; // $ffd6.3-0
  uint32_t romSize; // $ffd7 (0x400 << x)
  uint32_t ramSize; // $ffd8 (0x400 << x)
  uint8_t region; // $ffd9 (also NTSC/PAL)
  uint8_t maker; // $ffda ($33: header V3)
  uint8_t version; // $ffdb
  uint16_t checksumComplement; // $ffdc,$ffdd
  uint16_t checksum; // $ffde,$ffdf
  // v2/v3 (v2 only exCoprocessor)
  char makerCode[3]; // $ffb0,$ffb1: (2 chars + \0)
  char gameCode[5]; // $ffb2-$ffb5: (4 chars + \0)
  uint32_t flashSize; // $ffbc (0x400 << x)
  uint32_t exRamSize; // $ffbd (0x400 << x) (used for GSU?)
  uint8_t specialVersion; // $ffbe
  uint8_t exCoprocessor; // $ffbf (if coprocessor = $f)
  // calculated stuff
  int16_t score; // score for header, to see which mapping is most likely
  bool pal; // if this is a rom for PAL regions instead of NTSC
  uint8_t cartType; // calculated type
} CartHeader;

static void readHeader(const uint8_t* data, int length, int location, CartHeader* header);

bool snes_loadRom(Snes* snes, const uint8_t* data, int length) {
  // if smaller than smallest possible, don't load
  if(length < 0x8000) {
    printf("Failed to load rom: rom to small (%d bytes)\n", length);
    return false;
  }
  // check headers
  CartHeader headers[4];
  memset(headers, 0, sizeof(headers));
  for(int i = 0; i < 4; i++) {
    headers[i].score = -50;
  }
  if(length >= 0x8000) readHeader(data, length, 0x7fc0, &headers[0]);
  if(length >= 0x8200) readHeader(data, length, 0x81c0, &headers[1]);
  if(length >= 0x10000) readHeader(data, length, 0xffc0, &headers[2]);
  if(length >= 0x10200) readHeader(data, length, 0x101c0, &headers[3]);
  // see which it is
  int max = 0;
  int used = 0;
  for(int i = 0; i < 4; i++) {
    if(headers[i].score > max) {
      max = headers[i].score;
      used = i;
    }
  }
  if(used & 1) {
    // odd-numbered ones are for headered roms
    data += 0x200; // move pointer past header
    length -= 0x200; // and subtract from size
  }
  // check if we can load it
  if (headers[used].coprocessor == 1)
    headers[used].cartType = CART_SUPERFX;
  /* Nintendo SA-1 boards use coprocessor id $3. Their map modes are in the
   * $2x LoROM family (Super Mario RPG is $23/$35). */
  if (headers[used].coprocessor == 3)
    headers[used].cartType = CART_SA1;
  /* Coprocessor id $F means "see the $FFBF sub-type byte"; $10 there is
   * Capcom's Cx4 (Mega Man X2 / X3 — the only two Cx4 titles). The other $F
   * sub-types (SPC7110, ST010/011/018) are still unsupported. */
  if (headers[used].coprocessor == 0xf && headers[used].exCoprocessor == 0x10)
    headers[used].cartType = CART_CX4;
  /* The high cartridge-type nibble identifies the NEC DSP family, not the
   * firmware. Keep this mapper deliberately scoped to the $20/$03 signature
   * used by Super Mario Kart's SHVC-1K1X board; DSP-2/3/4 use related header
   * encodings but different firmware and/or maps. */
  bool has_nec_dsp = headers[used].coprocessor == 0 &&
                     (headers[used].chips == 3 || headers[used].chips == 5);
  bool has_smk_dsp1_map = has_nec_dsp && headers[used].speed == 2 &&
                          headers[used].type == 0 &&
                          headers[used].chips == 3;
  bool has_smk_title =
      strcmp(headers[used].name, "SUPER MARIO KART     ") == 0;
  bool has_smk_dsp1_hirom = has_nec_dsp && has_smk_title &&
                            headers[used].speed == 3 &&
                            headers[used].type == 1 &&
                            headers[used].chips == 5;
  if (has_smk_dsp1_map)
    headers[used].cartType = CART_DSP1;
  else if (has_smk_dsp1_hirom)
    headers[used].cartType = CART_DSP1_HIROM;
  else if (has_nec_dsp) {
    printf("Failed to load rom: unsupported NEC DSP board "
           "(map=%x, cartridge type=%02x)\n",
           (headers[used].speed << 4) | headers[used].type,
           (headers[used].coprocessor << 4) | headers[used].chips);
    return false;
  }
  if(headers[used].cartType > CART_SA1) {
    printf("Failed to load rom: unsupported type (%d)\n", headers[used].cartType);
    return false;
  }
if (headers[used].coprocessor == 0xf && headers[used].exCoprocessor != 0x10) {
     /* SPC7110 / ST010 / ST011 / ST018 are not modelled. Load as plain LoROM
      * so the failure surfaces through the off-rails ring (which names the
      * unbacked window) rather than as a bare refusal — but say so plainly, a
      * silent mismap is the worst outcome. */
     printf("Warning! Unmodelled custom coprocessor ($FFBF sub-type %02x) — "
            "its register window is unbacked; expect off-rails reads.\n",
            headers[used].exCoprocessor);
   }

   /* S-DD1 detection:
    * - Star Ocean: coprocessor=4, chips=5, V3 header (maker=0x33)
    * - Street Fighter Alpha 2 / Zero 2: coprocessor=4, chips=3, V3 header
    * Fallback: ROM name check (snes9x approach)
    */
   bool has_sdd1 = headers[used].coprocessor == 4 &&
                   (headers[used].chips == 5 || headers[used].chips == 3) &&
                   headers[used].maker == 0x33;

   bool has_sdd1_name = strncmp(headers[used].name, "STAR OCEAN", 10) == 0 ||
                        strncmp(headers[used].name, "STREET FIGHTER ALPHA2", 21) == 0 ||
                        strncmp(headers[used].name, "STREET FIGHTER ZERO2", 20) == 0;

   if (has_sdd1 || has_sdd1_name)
       headers[used].cartType = CART_SDD1;

   printf("[snes_load] name=%s coproc=%d chips=%d maker=%02x -> cartType=%d\n",
          headers[used].name,
          headers[used].coprocessor, headers[used].chips,
          headers[used].maker, headers[used].cartType);
  // expand to a power of 2
  int newLength = 0x8000;
  while(true) {
    if(length <= newLength) {
      break;
    }
    newLength *= 2;
  }
  uint8_t* newData = malloc(newLength);
  memcpy(newData, data, length);
  int test = 1;
  while(length != newLength) {
    if(length & test) {
      memcpy(newData + length, newData + length - test, test);
      length += test;
    }
    test *= 2;
  }
  // load it
  /* SRAM size. The header stores it as 0x400 << n, so n == 0 is ambiguous
   * between "1 KB" and "none"; hardware treats 0 as none. Cx4 carts declare
   * chipset ROM+COPRO with ramSize byte 0 and carry no battery at all — do not
   * hand them a phantom 1 KB SRAM mapped over banks $70-$7D. */
  int cart_ram_size;
  if (headers[used].cartType == CART_SUPERFX)
    cart_ram_size = headers[used].ramSize > 1024 ? headers[used].ramSize
                                                 : 64 * 1024;
  else if (headers[used].cartType == CART_CX4)
    cart_ram_size = 0;
  else if (headers[used].cartType == CART_DSP1 ||
           headers[used].cartType == CART_DSP1_HIROM)
    cart_ram_size = headers[used].ramSize > 1024 ? headers[used].ramSize : 0;
else if (headers[used].cartType == CART_SA1)
    cart_ram_size = headers[used].ramSize > 1024 ? headers[used].ramSize : 0;
  else if (headers[used].cartType == CART_SDD1)
    cart_ram_size = headers[used].ramSize > 1024 ? headers[used].ramSize : 0;
  else
    cart_ram_size = headers[used].chips > 0 ? headers[used].ramSize : 0;

#if SNESRECOMP_ENABLE_MODS
  if (cart_ram_size == 0) {
    uint32_t synthetic_sram_size = snes_mod_runtime_synthetic_sram_size_c();
    if (synthetic_sram_size > 0)
      cart_ram_size = (int)synthetic_sram_size;
  }
#endif

  cart_load(
    snes->cart, headers[used].cartType,
    newData, newLength,
    cart_ram_size
  );
  
  free(newData);
  return true;
}


static void readHeader(const uint8_t* data, int length, int location, CartHeader* header) {
  // read name, TODO: non-ASCII names?
  for(int i = 0; i < 21; i++) {
    uint8_t ch = data[location + i];
    if(ch >= 0x20 && ch < 0x7f) {
      header->name[i] = ch;
    } else {
      header->name[i] = '.';
    }
  }
  header->name[21] = 0;
  // read rest
  header->speed = data[location + 0x15] >> 4;
  header->type = data[location + 0x15] & 0xf;
  header->coprocessor = data[location + 0x16] >> 4;
  header->chips = data[location + 0x16] & 0xf;
  header->romSize = 0x400 << data[location + 0x17];
  header->ramSize = 0x400 << data[location + 0x18];
  header->region = data[location + 0x19];
  header->maker = data[location + 0x1a];
  header->version = data[location + 0x1b];
  header->checksumComplement = (data[location + 0x1d] << 8) + data[location + 0x1c];
  header->checksum = (data[location + 0x1f] << 8) + data[location + 0x1e];
  // read v3 and/or v2
  header->headerVersion = 1;
  if(header->maker == 0x33) {
    header->headerVersion = 3;
    // maker code
    for(int i = 0; i < 2; i++) {
      uint8_t ch = data[location - 0x10 + i];
      if(ch >= 0x20 && ch < 0x7f) {
        header->makerCode[i] = ch;
      } else {
        header->makerCode[i] = '.';
      }
    }
    header->makerCode[2] = 0;
    // game code
    for(int i = 0; i < 4; i++) {
      uint8_t ch = data[location - 0xe + i];
      if(ch >= 0x20 && ch < 0x7f) {
        header->gameCode[i] = ch;
      } else {
        header->gameCode[i] = '.';
      }
    }
    header->gameCode[4] = 0;
    header->flashSize = 0x400 << data[location - 4];
    header->exRamSize = 0x400 << data[location - 3];
    header->specialVersion = data[location - 2];
    header->exCoprocessor = data[location - 1];
  } else if(data[location + 0x14] == 0) {
    header->headerVersion = 2;
    header->exCoprocessor = data[location - 1];
  }
  // get region
  header->pal = (header->region >= 0x2 && header->region <= 0xc) || header->region == 0x11;
  header->cartType = location < 0x9000 ? 1 : 2;
  // get score
  // TODO: check name, maker/game-codes (if V3) for ASCII, more vectors,
  //   more first opcode, rom-sizes (matches?), type (matches header location?)
  int score = 0;
  score += (header->speed == 2 || header->speed == 3) ? 5 : -4;
  score += (header->type <= 3 || header->type == 5) ? 5 : -2;
  score += (header->coprocessor <= 5 || header->coprocessor >= 0xe) ? 5 : -2;
  score += (header->chips <= 6 || header->chips == 9 || header->chips == 0xa) ? 5 : -2;
  score += (header->region <= 0x14) ? 5 : -2;
  score += (header->checksum + header->checksumComplement == 0xffff) ? 8 : -6;
  uint16_t resetVector = data[location + 0x3c] | (data[location + 0x3d] << 8);
  score += (resetVector >= 0x8000) ? 8 : -20;
  // check first opcode after reset
  int opcodeLoc = location + 0x40 - 0x8000 + (resetVector & 0x7fff);
  uint8_t opcode = 0xff;
  if(opcodeLoc < length) {
    opcode = data[opcodeLoc];
  } else {
    score -= 14;
  }
  if(opcode == 0x78 || opcode == 0x18) {
    // sei, clc (for clc:xce)
    score += 6;
  }
  if(opcode == 0x4c || opcode == 0x5c || opcode == 0x9c) {
    // jmp abs, jml abl, stz abs
    score += 3;
  }
  if(opcode == 0x00 || opcode == 0xff || opcode == 0xdb) {
    // brk, sbc alx, stp
    score -= 6;
  }
  header->score = score;
}
