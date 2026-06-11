/* ppu_dma_trace.c — always-on PPU + DMA observability ring.
 * See ppu_dma_trace.h for the rationale (record history, never arm-at-probe). */

#include "ppu_dma_trace.h"
#include "snes/ppu.h"
#include "common_rtl.h"   /* g_ppu */
#include "cpu_state.h"    /* g_cpu (65816 S) */

#include <stdlib.h>
#include <string.h>

extern CpuState g_cpu;
extern uint8_t  g_ram[0x20000];

#define DMA_RING_LEN  8192   /* ~16 B each; covers many frames of uploads */
#define PPU_RING_LEN  4096   /* one snapshot/frame => ~68 s at 60 fps      */
#define DMA_DUMP_MAX  512    /* slice size pulled into the post-mortem     */

typedef struct {
  int      frame;
  uint32_t seq;
  uint8_t  channel;
  uint8_t  fromB;   /* 1 = B->A, 0 = A->B (memory -> PPU)          */
  uint8_t  aBank;
  uint8_t  bAdr;    /* B-bus dest reg low byte: 18/19=VRAM,22=CGRAM,04=OAM */
  uint16_t aAdr;
  uint16_t size;    /* 0 encodes a full 0x10000 transfer            */
} DmaEvent;

typedef struct {
  int      frame;
  uint8_t  inidisp;    /* bit7 = forced blank, bits0-3 = brightness */
  uint8_t  tm;         /* main-screen layer/OBJ enable ($212C)      */
  uint8_t  ts;         /* sub-screen layer/OBJ enable ($212D)       */
  uint8_t  bgmode;     /* $2105                                     */
  uint16_t cgram_nz;   /* non-zero CGRAM entries (0..256)           */
  uint32_t vram_nz;    /* non-zero VRAM words (0..0x8000)           */
  uint16_t dma_a2b;    /* A->B DMAs recorded during this frame      */
  uint16_t s_reg;      /* 65816 stack pointer at end-of-frame       */
  uint8_t  game_state; /* $7E:0998 (SM kGameState_*)                */
  uint8_t  game_mode;  /* $7E:0100 (SM GameMode)                    */
  uint16_t wram_probe[PPUDMA_WRAM_PROBE_MAX]; /* see ppudma_wram_probe */
} PpuSnap;

/* Always-on per-frame WRAM word probes. SNESRECOMP_HEARTBEAT_WRAM is a
 * comma-separated list of up to PPUDMA_WRAM_PROBE_MAX hex WRAM offsets
 * (e.g. "1F51,0998"); each is captured into every PpuSnap from frame 0
 * (env parsed on first snapshot — before the game's first frame runs),
 * streamed on the heartbeat line, and dumped with the ring. The probe
 * SET is configuration; the capture itself is always-on history. */
static uint32_t s_wram_probe_addr[PPUDMA_WRAM_PROBE_MAX];
static int s_wram_probe_n = -1;

static void wram_probe_init(void) {
  s_wram_probe_n = 0;
  const char *v = getenv("SNESRECOMP_HEARTBEAT_WRAM");
  if (!v || !v[0]) return;
  while (*v && s_wram_probe_n < PPUDMA_WRAM_PROBE_MAX) {
    char *end = NULL;
    unsigned long a = strtoul(v, &end, 16);
    if (end == v) break;
    s_wram_probe_addr[s_wram_probe_n++] = (uint32_t)(a & 0x1FFFF);
    v = (*end == ',') ? end + 1 : end;
    if (!*end) break;
  }
}

static DmaEvent s_dma_ring[DMA_RING_LEN];
static uint64_t s_dma_widx;
static PpuSnap  s_ppu_ring[PPU_RING_LEN];
static uint64_t s_ppu_widx;
static uint16_t s_dma_this_frame;

static int env_int(const char *name) {
  const char *v = getenv(name);
  return (v && v[0]) ? atoi(v) : 0;
}

void ppudma_record_dma(int channel, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size) {
  extern int snes_frame_counter;
  DmaEvent *e = &s_dma_ring[s_dma_widx % DMA_RING_LEN];
  e->frame   = snes_frame_counter;
  e->seq     = (uint32_t)s_dma_widx;
  e->channel = (uint8_t)channel;
  e->fromB   = (uint8_t)(fromB != 0);
  e->aBank   = aBank;
  e->bAdr    = bAdr;
  e->aAdr    = aAdr;
  e->size    = size;
  s_dma_widx++;
  if (!fromB) s_dma_this_frame++;

  static int log_init = 0, log_on = 0;
  if (!log_init) { log_on = env_int("SNESRECOMP_DMA_LOG"); log_init = 1; }
  if (log_on) {
    fprintf(stderr, "[dma] f%d ch%d %s src=%02X:%04X dst=$21%02X size=%u\n",
            snes_frame_counter, channel, fromB ? "B->A" : "A->B",
            (unsigned)aBank, (unsigned)aAdr, (unsigned)bAdr,
            (unsigned)(size ? size : 0x10000u));
  }
}

void ppudma_frame_snapshot(int frame) {
  Ppu *p = g_ppu;
  if (!p) { s_dma_this_frame = 0; return; }

  PpuSnap *s = &s_ppu_ring[s_ppu_widx % PPU_RING_LEN];
  s->frame   = frame;
  s->inidisp = p->inidisp;
  s->tm      = p->screenEnabled[0];
  s->ts      = p->screenEnabled[1];
  s->bgmode  = p->bgmode;

  uint16_t cnz = 0;
  for (int i = 0; i < 0x100; i++) if (p->cgram[i]) cnz++;
  s->cgram_nz = cnz;

  uint32_t vnz = 0;
  for (int i = 0; i < 0x8000; i++) if (p->vram[i]) vnz++;
  s->vram_nz = vnz;

  s->dma_a2b = s_dma_this_frame;
  s->s_reg = g_cpu.S;
  s->game_state = g_ram[0x0998];   /* $7E:0998 */
  s->game_mode  = g_ram[0x0100];   /* $7E:0100 */
  if (s_wram_probe_n < 0) wram_probe_init();
  for (int i = 0; i < s_wram_probe_n; i++) {
    uint32_t a = s_wram_probe_addr[i];
    s->wram_probe[i] =
        (uint16_t)(g_ram[a] | ((uint32_t)g_ram[(a + 1) & 0x1FFFF] << 8));
  }
  s_ppu_widx++;

  static int hb_init = 0, hb_n = 0;
  if (!hb_init) { hb_n = env_int("SNESRECOMP_PPU_HEARTBEAT"); hb_init = 1; }
  if (hb_n > 0 && (frame % hb_n) == 0) {
    fprintf(stderr,
      "[ppu] f%d inidisp=%02X(%s bri=%u) TM=%02X bgmode=%u "
      "cgram_nz=%u vram_nz=%u dma_a2b=%u S=%04X gs=%02X gm=%02X",
      frame, (unsigned)s->inidisp,
      (s->inidisp & 0x80) ? "BLANK" : "on", (unsigned)(s->inidisp & 0xf),
      (unsigned)s->tm, (unsigned)(s->bgmode & 7),
      (unsigned)s->cgram_nz, (unsigned)s->vram_nz, (unsigned)s->dma_a2b,
      (unsigned)s->s_reg, (unsigned)s->game_state, (unsigned)s->game_mode);
    for (int i = 0; i < s_wram_probe_n; i++) {
      fprintf(stderr, " [%04X]=%04X", (unsigned)s_wram_probe_addr[i],
              (unsigned)s->wram_probe[i]);
    }
    fprintf(stderr, "\n");
  }

  /* First time the stack pointer leaves the sane SM range ($0000-$1FFF),
   * announce it once: this brackets the control-flow divergence to a frame. */
  static int s_wild_announced = 0;
  if (!s_wild_announced && s->s_reg > 0x1FFF) {
    fprintf(stderr,
      "[ppu] *** WILD STACK: frame %d S=%04X (was sane until here) "
      "gs=%02X gm=%02X ***\n",
      frame, (unsigned)s->s_reg, (unsigned)s->game_state,
      (unsigned)s->game_mode);
    s_wild_announced = 1;
  }

  s_dma_this_frame = 0;
}

void ppudma_dump_json(FILE *f) {
  /* Per-frame PPU snapshots (oldest-first within the retained window). */
  uint64_t pw = s_ppu_widx;
  int pn = PPU_RING_LEN;
  if ((uint64_t)pn > pw) pn = (int)pw;
  fprintf(f, "  \"ppu_frames\": {\"write_idx\": %llu, \"snaps\": [",
          (unsigned long long)pw);
  for (int i = 0; i < pn; i++) {
    uint64_t off = pw - pn + i;
    const PpuSnap *s = &s_ppu_ring[off % PPU_RING_LEN];
    fprintf(f,
      "%s{\"frame\":%d,\"inidisp\":%u,\"forced_blank\":%u,\"brightness\":%u,"
      "\"tm\":%u,\"ts\":%u,\"bgmode\":%u,\"cgram_nz\":%u,\"vram_nz\":%u,"
      "\"dma_a2b\":%u,\"s_reg\":%u,\"game_state\":%u,\"game_mode\":%u}",
      (i ? "," : ""), s->frame, (unsigned)s->inidisp,
      (unsigned)((s->inidisp & 0x80) != 0), (unsigned)(s->inidisp & 0xf),
      (unsigned)s->tm, (unsigned)s->ts, (unsigned)(s->bgmode & 7),
      (unsigned)s->cgram_nz, (unsigned)s->vram_nz, (unsigned)s->dma_a2b,
      (unsigned)s->s_reg, (unsigned)s->game_state, (unsigned)s->game_mode);
  }
  fprintf(f, "]},\n");

  /* Configured WRAM probes (one array per probe, frame-aligned with the
   * snaps above). Empty when SNESRECOMP_HEARTBEAT_WRAM is unset. */
  fprintf(f, "  \"wram_probes\": {");
  for (int j = 0; j < (s_wram_probe_n < 0 ? 0 : s_wram_probe_n); j++) {
    fprintf(f, "%s\"0x%04X\": [", j ? "," : "",
            (unsigned)s_wram_probe_addr[j]);
    for (int i = 0; i < pn; i++) {
      uint64_t off = pw - pn + i;
      const PpuSnap *s = &s_ppu_ring[off % PPU_RING_LEN];
      fprintf(f, "%s%u", (i ? "," : ""), (unsigned)s->wram_probe[j]);
    }
    fprintf(f, "]");
  }
  fprintf(f, "},\n");

  /* Most-recent DMA events. */
  uint64_t dw = s_dma_widx;
  int dn = DMA_RING_LEN;
  if ((uint64_t)dn > dw) dn = (int)dw;
  if (dn > DMA_DUMP_MAX) dn = DMA_DUMP_MAX;
  fprintf(f, "  \"dma_events\": {\"write_idx\": %llu, \"events\": [",
          (unsigned long long)dw);
  for (int i = 0; i < dn; i++) {
    uint64_t off = dw - dn + i;
    const DmaEvent *e = &s_dma_ring[off % DMA_RING_LEN];
    fprintf(f,
      "%s{\"seq\":%u,\"frame\":%d,\"ch\":%u,\"dir\":\"%s\","
      "\"src\":%u,\"dst_reg\":%u,\"size\":%u}",
      (i ? "," : ""), (unsigned)e->seq, e->frame, (unsigned)e->channel,
      e->fromB ? "B2A" : "A2B",
      (unsigned)(((uint32_t)e->aBank << 16) | e->aAdr),
      (unsigned)(0x2100 | e->bAdr),
      (unsigned)(e->size ? e->size : 0x10000u));
  }
  fprintf(f, "]},\n");
}
