#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "recomp_hw.h"
#include "framedump.h"
#include "smw_spc_player.h"
#include "util.h"
#include "config.h"
#include "snes/snes.h"
#include "debug_server.h"

uint8 g_ram[0x20000];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
bool g_did_finish_level_hook;
Ppu *g_ppu;
Dma *g_dma;

// FILE-backed SaveLoadInfo. snes_saveload calls back into func() once per
// scalar/blob; we route each call to fread/fwrite. Single magic+version
// header lets future format changes be detected.
#define RTL_SAV_MAGIC   0x52544c53u  /* "RTLS" */
#define RTL_SAV_VERSION 1u

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
  snes_reset(g_snes, true);
  // The real ROM's reset vector sets up CPU state (see common_cpu_infra.c).
  g_cpu->e = false;
  g_cpu->sp = 0x01FF;
  g_cpu->dp = 0;
  g_cpu->mf = false;
  g_cpu->xf = false;
  g_cpu->d = false;
  g_cpu->i = true;
  cpu_setFlags(g_cpu, cpu_getFlags(g_cpu));
  ppu_reset(g_ppu);
  if (!(mode & 1))
    memset(g_sram, 0, g_sram_size);

  RtlApuLock();
  g_spc_player->initialize(g_spc_player);
  RtlApuUnlock();
}

bool RtlRunFrame(uint32 inputs) {
  // g_did_finish_level_hook is still set by recompiled level-end paths
  // for telemetry / debug bookkeeping; we just clear it here.
  g_did_finish_level_hook = false;

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
  extern const char *g_last_recomp_func;
  if (!(addr & 0x8000) || addr >= 0x7e0000) {
    printf("RomPtr - Invalid access 0x%x in %s!\n", addr, g_last_recomp_func ? g_last_recomp_func : "?");
    if (!g_fail) {
      g_fail = true;
    }
  }
  return (uint8 *)&g_rom[(((addr >> 16) << 15) | (addr & 0x7fff)) & 0x3fffff];
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
    // APU read — need emulator for this since APU is emulated
    return snes_read(g_snes, reg);
  } else if (reg == 0x2180) {
    return snes_readBBus(g_snes, reg & 0xff);
  } else if (reg >= 0x4200 && reg < 0x4220) {
    return recomp_read_internal_reg(reg);
  } else if (reg >= 0x4300 && reg < 0x4380) {
    return dma_read(g_dma, reg);
  }
  return 0;
}

uint16 ReadRegWord(uint16 reg) {
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
  WriteReg(reg, (uint8)value);
  WriteReg(reg + 1, value >> 8);
}

uint8 *IndirPtr_Slow(LongPtr ptr, uint16 offs) {
  return IndirPtr(ptr, offs);  /* delegates to inline version in header */
}

/* IndirWriteByte is now inline in common_rtl.h */

void RtlApuWrite(uint16 adr, uint8 val) {
  assert(adr >= APUI00 && adr <= APUI03);
  // Catch the APU up to the current cycle and write the port value
  // directly. Serialise with the audio thread via RtlApuLock — it
  // holds the same lock while cycling the APU in RtlRenderAudio.
  RtlApuLock();
  g_snes->apuCatchupCycles = 32;
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
