#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "recomp_hw.h"
#include "framedump.h"
#include "util.h"
#include "config.h"
#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "debug_server.h"

uint8 g_ram[0x20000];
/* Diag flag — set via debug_server `force_apu_bbaa <0|1>` cmd. When 1,
 * every 16-bit read of $2140 returns $BBAA. Useful for proving the
 * host-side compare logic is correct. */
int g_force_apu_bbaa = 0;
/* Brutal hack: when 1, ALL APU port reads return a value derived from
 * the v2 CpuState's A register so polls always succeed. Lets us see
 * how much of the rest of the recompiled boot path works when the SPC
 * handshake is short-circuited. TEMP default-on for visual boot test —
 * remove once real SPC engine handshake works. */
int g_apu_autoack = 1;
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;

// Main-CPU cycle estimate, incremented per RDB_BLOCK_HOOK in debug_on_block_enter.
// Used to pace APU catchup realistically: real SNES is ~3.58 MHz main / ~1.024 MHz APU,
// ratio ~3.5:1. Prior code hardcoded apuCatchupCycles=32 per APU port touch regardless
// of elapsed main-CPU time, which let APU stay artificially synchronized -- SMW boot's
// "wait for APU ack" loops resolved instantly, racing through ~200 frames worth of game
// logic in ~95 frames. Tracking real elapsed cycles makes those waits actually wait.
uint64_t g_main_cpu_cycles_estimate = 0;
uint64_t g_apu_last_sync_cycles = 0;

// FILE-backed SaveLoadInfo. snes_saveload calls back into func() once per
// scalar/blob; we route each call to fread/fwrite. Single magic+version
// header lets future format changes be detected.
#define RTL_SAV_MAGIC   0x52544c53u  /* "RTLS" */
#define RTL_SAV_VERSION 4u  /* v4: dropped Dma.pad[7] blob tail */

typedef struct FileSli {
  SaveLoadInfo base;
  FILE *f;
  bool is_save;
  bool error;
} FileSli;

static void file_sli_func(SaveLoadInfo *sli, void *data, size_t n) {
  FileSli *fs = (FileSli *)sli;
  if (fs->error) return;
  size_t got = fs->is_save ? fwrite(data, 1, n, fs->f)
                           : fread(data, 1, n, fs->f);
  if (got != n) fs->error = true;
}

void RtlReset(int mode) {
  snes_frame_counter = 0;
  g_main_cpu_cycles_estimate = 0;
  g_apu_last_sync_cycles = 0;
  snes_reset(g_snes, true);
  SnesEnterNativeMode();
  ppu_reset(g_ppu);
  if (!(mode & 1))
    memset(g_sram, 0, g_sram_size);

  RtlApuLock();
  g_spc_player->initialize(g_spc_player);
  RtlApuUnlock();
}

bool RtlRunFrame(uint32 inputs) {
  // Avoid up/down and left/right from being pressed at the same time
  if ((inputs & 0x30) == 0x30) inputs ^= 0x30;
  if ((inputs & 0xc0) == 0xc0) inputs ^= 0xc0;
  // Player2
  if ((inputs & 0x30000) == 0x30000) inputs ^= 0x30000;
  if ((inputs & 0xc0000) == 0xc0000) inputs ^= 0xc0000;

  g_snes->input1_currentState = inputs & 0xfff;
  g_snes->input2_currentState = (inputs >> 12) & 0xfff;

  WatchdogFrameStart();
  g_rtl_game_info->run_frame();
  if (g_framedump_callback)
    g_framedump_callback(snes_frame_counter, g_ram);
  {
    extern void debug_server_record_frame(int);
    debug_server_record_frame(snes_frame_counter);
  }

  snes_frame_counter++;
  return false;
}

void RtlSaveSnapshot(const char *filename) {
  FILE *f = fopen(filename, "wb");
  if (!f) {
    printf("Failed fopen for save: %s\n", filename);
    return;
  }
  uint32 hdr[2] = { RTL_SAV_MAGIC, RTL_SAV_VERSION };
  fwrite(hdr, sizeof(hdr), 1, f);
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, true, false };
  snes_saveload(g_snes, &fs.base);
  RtlApuUnlock();
  if (fs.error) printf("Save write error: %s\n", filename);
  fclose(f);
}

bool RtlLoadSnapshot(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (!f)
    return false;
  uint32 hdr[2];
  if (fread(hdr, sizeof(hdr), 1, f) != 1
      || hdr[0] != RTL_SAV_MAGIC || hdr[1] != RTL_SAV_VERSION) {
    printf("Save file %s: bad magic/version (legacy StateRecorder format no longer supported)\n", filename);
    fclose(f);
    return false;
  }
  RtlApuLock();
  FileSli fs = { { &file_sli_func }, f, false, false };
  snes_saveload(g_snes, &fs.base);
  RtlApuUnlock();
  fclose(f);
  if (fs.error) {
    printf("Save read error: %s\n", filename);
    return false;
  }
  return true;
}

void RtlSaveLoad(int cmd, int slot) {
  char name[128];
  const char *prefix = g_rtl_game_info->save_name_prefix;
  if (prefix)
    sprintf(name, "saves/%s%d.sav", prefix, slot);
  else
    sprintf(name, "saves/%s_save%d.sav", g_rtl_game_info->title, slot);
  printf("*** %s slot %d: %s\n",
    cmd == kSaveLoad_Save ? "Saving" : "Loading", slot, name);
  if (cmd == kSaveLoad_Save)
    RtlSaveSnapshot(name);
  else
    RtlLoadSnapshot(name);
}


void MemCpy(void *dst, const void *src, int size) {
  memcpy(dst, src, size);
}

bool Unreachable(void) {
  printf("Unreachable!\n");
  assert(0);
  g_ram[0x1ffff] = 1;
  return false;
}

uint8 *RomPtr(uint32_t addr) {
  if (!(addr & 0x8000) || addr >= 0x7e0000) {
    if (!g_fail) g_fail = true;
    /* No printf — the ring buffer + cpu_trace_offrails is the
     * channel for backwards investigation. printf'ing every bad
     * read floods stderr with millions of identical lines. */
    cpu_trace_offrails("RomPtr-invalid", addr);
  }
  /* Compute LoROM offset, then mirror against ACTUAL ROM size. SMW is
   * 512KB but the original `& 0x3fffff` mask assumed 4MB, so reads at
   * high banks (e.g. $FF:0100 — bogus pointer values from data-as-code
   * regions or unmapped ARAM) computed index 0x7F8100, FAR past
   * g_rom's 0x80000 bytes — instant SIGSEGV. The right behaviour:
   * mirror to actual ROM size, matching real SNES bank-mirroring. */
  extern Snes *g_snes;
  uint32_t off = (((addr >> 16) << 15) | (addr & 0x7fff));
  uint32_t rom_size = g_snes && g_snes->cart ? (uint32_t)g_snes->cart->romSize : 0x80000;
  if (rom_size == 0) rom_size = 0x80000;
  return (uint8 *)&g_rom[off % rom_size];
}

// MVN/MVP block-move pointer: resolves (bank, addr) per 65816 LoROM rules.
// Banks $00-$3F and $80-$BF mirror WRAM at $0000-$1FFF; $7E/$7F are WRAM.
// Everything else is ROM (same mapping as RomPtr). Returns a non-const pointer
// because MVN dst writes through this; callers must only dst into WRAM banks.
uint8 *MvnPtr(uint8_t bank, uint16_t addr) {
  if (bank == 0x7E) return g_ram + addr;
  if (bank == 0x7F) return g_ram + 0x10000 + addr;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && addr < 0x2000)
    return g_ram + addr;
  uint32_t full = ((uint32_t)bank << 16) | addr;
  return (uint8 *)&g_rom[(((full >> 16) << 15) | (full & 0x7fff)) & 0x3fffff];
}

// Replay a DMA transfer into g_ppu after the emulator executed it into g_snes->ppu.

static int _writereg_ppu_count = 0;
static int _writereg_dma_count = 0;
void WriteReg(uint16 reg, uint8 value) {
  // Direct dispatch — bypass emulator bus
  if (reg >= 0x2100 && reg < 0x2140) {
    ppu_write(g_ppu, reg & 0xff, value);
  } else if (reg >= 0x2140 && reg < 0x2180) {
    RtlApuWrite(reg, value);
  } else if (reg >= 0x2180 && reg < 0x2184) {
    snes_writeBBus(g_snes, reg & 0xff, value);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    recomp_write_internal_reg(reg, value);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    dma_write(g_dma, reg, value);
  }
  debug_server_on_reg_write(reg, value);
}


uint8 ReadReg(uint16 reg) {
  // Direct dispatch — bypass emulator bus
  if (reg >= 0x2100 && reg < 0x2140) {
    return ppu_read(g_ppu, reg & 0xff);
  } else if (reg >= 0x2140 && reg < 0x2180) {
    if (g_apu_autoack) {
      /* Auto-ack: return v2 CpuState's current A low byte for $2140
       * (which is what L_80D3 / L_80AA / L_809A wait for), and BB/AA
       * for $2141 to keep the BBAA poll satisfied. */
      extern struct CpuState g_cpu;
      if (reg == 0x2140) return (uint8)(g_cpu.A & 0xFF);
      if (reg == 0x2141) return 0xBB;
      return 0;
    }
    // APU read — need emulator for this since APU is emulated
    return snes_read(g_snes, reg);
  } else if (reg == 0x2180) {
    return snes_readBBus(g_snes, reg & 0xff);
  } else if (reg == 0x4016 || reg == 0x4017) {
    /* JOYSER0 / JOYSER1 — manual joypad-read serial registers.
     * Routed through snes_readReg so the SNES core can return the
     * controller-presence signature (bit 0 set after the implicit
     * "16 reads done" state). Phase B koopa-stomp investigation
     * (2026-04-24) found these reads were falling through to the
     * default `return 0` and breaking SMW's CheckWhichControllers-
     * ArePluggedIn detection. */
    return snes_readReg(g_snes, reg);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    return recomp_read_internal_reg(reg);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    return dma_read(g_dma, reg);
  }
  return 0;
}

uint16 ReadRegWord(uint16 reg) {
  // APU port quirk: 16-bit CMP $2140 must see a CONSISTENT outPorts
  // snapshot. Two separate ReadReg calls would each catch the APU
  // up — between them the SPC could write only the LO byte (port 0)
  // before host has read HI (port 1), so host sees a torn value. Read
  // both ports atomically (single catchup) for the APU-port range.
  if (reg >= 0x2140 && reg <= 0x217F) {
    extern void rtl_accumulate_apu_catchup(void);
    void RtlApuLock(void); void RtlApuUnlock(void);
    void snes_catchupApu(Snes* snes);
    extern Snes *g_snes;
    extern int g_force_apu_bbaa;
    extern int g_apu_autoack;
    extern struct CpuState g_cpu;
    if (g_apu_autoack && reg == 0x2140) {
      /* Auto-ack 16-bit: return $BBAA always for the BBAA poll. */
      return 0xBBAA;
    }
    RtlApuLock();
    rtl_accumulate_apu_catchup();
    snes_catchupApu(g_snes);
    uint8_t lo = g_snes->apu->outPorts[(reg & 0x3)];
    uint8_t hi = g_snes->apu->outPorts[((reg + 1) & 0x3)];
    RtlApuUnlock();
    if (g_force_apu_bbaa && reg == 0x2140) {
      lo = 0xAA;
      hi = 0xBB;
    }
    return (uint16_t)lo | ((uint16_t)hi << 8);
  }
  uint16_t rv = ReadReg(reg);
  rv |= ReadReg(reg + 1) << 8;
  return rv;
}

static void WriteVramWord(Ppu *ppu, uint16 value) {
  uint16_t adr = ppu->vramPointer;
  ppu->vram[adr & 0x7fff] = value;
  debug_server_on_vram_write(adr & 0x7fff, value);
  ppu->vramPointer += ppu->vramIncrement;
}

void WriteRegWord(uint16 reg, uint16 value) {
  if (reg == 0x2118) {
    // VRAM data port: atomic word write
    WriteVramWord(g_ppu, value);
    return;
  }
  // APU port quirk: 16-bit STA $2140 transfers data via $2141 (hi)
  // and the ack-trigger via $2140 (lo). On real hardware both bytes
  // hit the bus together; SMW's SPC IPL upload protocol reads $2141
  // the moment it sees $2140 change. If we write lo first the IPL
  // latches stale $2141. Order hi-then-lo so $2141 is in place
  // before $2140 fires the trigger.
  if (reg >= 0x2140 && reg <= 0x217F) {
    WriteReg(reg + 1, value >> 8);
    WriteReg(reg, (uint8)value);
    return;
  }
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

uint8 *IndirPtr_Slow(LongPtr ptr, uint16 offs) {
  return IndirPtr(ptr, offs);  /* delegates to inline version in header */
}

/* IndirWriteByte is now inline in common_rtl.h */

// Convert main-CPU cycle delta into APU cycles (ratio ~3.5:1) and accumulate
// into apuCatchupCycles. Caller holds RtlApuLock and is responsible for the
// snes_catchupApu() call. Sets g_apu_last_sync_cycles to the current main-CPU
// estimate so subsequent calls only see incremental work.
//
// Public so snes.c's snes_readBBus (the APU read path) can use the same
// pacing -- both reads and writes need to advance APU.
void rtl_accumulate_apu_catchup(void) {
  uint64_t delta = g_main_cpu_cycles_estimate - g_apu_last_sync_cycles;
  g_apu_last_sync_cycles = g_main_cpu_cycles_estimate;
  // 2/7 ≈ 1/3.5 (main MHz / APU MHz). Floor of zero is fine -- short deltas
  // (back-to-back APU touches with no block hooks between them) just don't
  // advance APU on this pass; cycles accumulate for the next touch.
  g_snes->apuCatchupCycles += (double)delta * 2.0 / 7.0;
}

void RtlApuWrite(uint16 adr, uint8 val) {
  assert(adr >= APUI00 && adr <= APUI03);
  // Catch the APU up to the current cycle and write the port value
  // directly. Serialise with the audio thread via RtlApuLock — it
  // holds the same lock while cycling the APU in RtlRenderAudio.
  RtlApuLock();
  rtl_accumulate_apu_catchup();
  snes_catchupApu(g_snes);
  g_snes->apu->inPorts[adr & 0x3] = val;
  RtlApuUnlock();
}

void RtlRenderAudio(int16 *audio_buffer, int samples, int channels) {
  assert(channels == 2);
  RtlApuLock();
  // Cycle the APU to fill the DSP sample buffer, then drain samples.
  // RtlApuLock is held throughout — matches the lock acquired by
  // RtlApuWrite / snes_readBBus on the CPU thread so both threads
  // agree on APU state.
  while (g_snes->apu->dsp->sampleOffset < 534)
    apu_cycle(g_snes->apu);
  dsp_getSamples(g_snes->apu->dsp, audio_buffer, samples);
  RtlApuUnlock();
}

void RtlReadSram(void) {
  char filename[64];
  snprintf(filename, sizeof(filename), "saves/%s.srm", g_rtl_game_info->title);
  FILE *f = fopen(filename, "rb");
  if (f) {
    if (fread(g_sram, 1, g_sram_size, f) != g_sram_size)
      fprintf(stderr, "Error reading %s\n", filename);
    fclose(f);
  }
}

void RtlWriteSram(void) {
  char filename[64], filename_bak[64];
  snprintf(filename, sizeof(filename), "saves/%s.srm", g_rtl_game_info->title);
  snprintf(filename_bak, sizeof(filename_bak), "saves/%s.srm.bak", g_rtl_game_info->title);
  rename(filename, filename_bak);
  FILE *f = fopen(filename, "wb");
  if (f) {
    fwrite(g_sram, 1, g_sram_size, f);
    fclose(f);
  } else {
    fprintf(stderr, "Unable to write %s\n", filename);
  }
}static const uint8 *SimpleHdma_GetPtr(uint32 p) {
  uint8 bank = (uint8)(p >> 16);
  uint16 addr = (uint16)(p & 0xffff);
  if (bank == 0x7E) return g_ram + addr;
  if (bank == 0x7F) return g_ram + 0x10000 + addr;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) && addr < 0x2000)
    return g_ram + addr;
  return RomPtr(p);
}

void SimpleHdma_Init(SimpleHdma *c, DmaChannel *dc) {
  if (!dc->hdmaActive) {
    c->table = 0;
    return;
  }
  c->table = SimpleHdma_GetPtr(dc->aAdr | dc->aBank << 16);
  c->rep_count = 0;
  c->mode = dc->mode | dc->indirect << 6;
  c->ppu_addr = dc->bAdr;
  c->indir_bank = dc->indBank;
}

void SimpleHdma_DoLine(SimpleHdma *c) {
  static const uint8 bAdrOffsets[8][4] = {
    {0, 0, 0, 0},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1},
    {0, 1, 2, 3},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1}
  };
  static const uint8 transferLength[8] = {
    1, 2, 2, 4, 4, 4, 2, 4
  };

  if (c->table == NULL)
    return;
  bool do_transfer = false;
  if ((c->rep_count & 0x7f) == 0) {
    c->rep_count = *c->table++;
    if (c->rep_count == 0) {
      c->table = NULL;
      return;
    }
    if(c->mode & 0x40) {
      c->indir_ptr = SimpleHdma_GetPtr(c->indir_bank << 16 | c->table[0] | c->table[1] * 256);
      c->table += 2;
    }
    do_transfer = true;
  }
  if(do_transfer || c->rep_count & 0x80) {
    for(int j = 0, j_end = transferLength[c->mode & 7]; j < j_end; j++) {
      uint8 v = c->mode & 0x40 ? *c->indir_ptr++ : *c->table++;
      uint16 addr = 0x2100 + c->ppu_addr + bAdrOffsets[c->mode & 7][j];
      ppu_write(g_ppu, addr, v);
      debug_server_on_reg_write(addr, v);
    }
  }
  c->rep_count--;
}
