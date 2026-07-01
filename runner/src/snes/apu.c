
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


#include "apu.h"
#include "snes.h"
#include "spc.h"
#include "dsp.h"
#include "../audio_trace.h"

static const uint8_t bootRom[0x40] = {
  0xcd, 0xef, 0xbd, 0xe8, 0x00, 0xc6, 0x1d, 0xd0, 0xfc, 0x8f, 0xaa, 0xf4, 0x8f, 0xbb, 0xf5, 0x78,
  0xcc, 0xf4, 0xd0, 0xfb, 0x2f, 0x19, 0xeb, 0xf4, 0xd0, 0xfc, 0x7e, 0xf4, 0xd0, 0x0b, 0xe4, 0xf5,
  0xcb, 0xf4, 0xd7, 0x00, 0xfc, 0xd0, 0xf3, 0xab, 0x01, 0x10, 0xef, 0x7e, 0xf4, 0x10, 0xeb, 0xba,
  0xf6, 0xda, 0x00, 0xba, 0xf4, 0xc4, 0xf4, 0xdd, 0x5d, 0xd0, 0xdb, 0x1f, 0x00, 0x00, 0xc0, 0xff
};

Apu* apu_init(void) {
  Apu* apu = calloc(1, sizeof(Apu));  /* zero padding: saveload/co-sim hash determinism */
  apu->spc = spc_init(apu);
  apu->dsp = dsp_init(apu->ram);
  apu_clearPortQueue(apu);
  return apu;
}

void apu_free(Apu* apu) {
  spc_free(apu->spc);
  dsp_free(apu->dsp);
  free(apu);
}

void apu_reset(Apu* apu) {
  apu->romReadable = true; // before resetting spc, because it reads reset vector from it
  spc_reset(apu->spc);
  dsp_reset(apu->dsp);
  memset(apu->ram, 0, sizeof(apu->ram));
  apu->dspAdr = 0;
  apu->cycles = 0;
  memset(apu->inPorts, 0, sizeof(apu->inPorts));
  memset(apu->outPorts, 0, sizeof(apu->outPorts));
  for(int i = 0; i < 3; i++) {
    apu->timer[i].cycles = 0;
    apu->timer[i].divider = 0;
    apu->timer[i].target = 0;
    apu->timer[i].counter = 0;
    apu->timer[i].enabled = false;
  }
  apu->cpuCyclesLeft = 7;
  apu_clearPortQueue(apu);
}

void apu_clearPortQueue(Apu* apu) {
  apu->portQHead = apu->portQTail = 0;
}

static void apu_applyPortWrite(Apu* apu, const ApuPortWrite *w) {
  apu->inPorts[w->port & 3] = w->val;
  audio_trace_on_cpu_port_apply(w->port, w->val);
}

void apu_schedulePortWrite(Apu* apu, uint8_t port, uint8_t val,
                           uint64_t target_sample) {
  if (apu->portQTail - apu->portQHead >= APU_PORT_QUEUE_LEN) {
    /* Full — apply the oldest immediately so ordering survives. */
    apu_applyPortWrite(apu, &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)]);
    apu->portQHead++;
  }
  ApuPortWrite *w = &apu->portQueue[apu->portQTail & (APU_PORT_QUEUE_LEN - 1)];
  w->target_sample = target_sample;
  w->port = (uint8_t)(port & 3);
  w->val = val;
  apu->portQTail++;
}

/* Apply every queued write whose target the produced-sample clock has
 * reached. Called at each DSP sample boundary inside apu_cycle. */
static void apu_drainPortQueue(Apu* apu) {
  uint64_t produced;
  audio_trace_sample_clocks(&produced, NULL);
  while (apu->portQHead != apu->portQTail) {
    ApuPortWrite *w = &apu->portQueue[apu->portQHead & (APU_PORT_QUEUE_LEN - 1)];
    if (w->target_sample > produced)
      break;
    apu_applyPortWrite(apu, w);
    apu->portQHead++;
  }
}

void apu_saveload(Apu *apu, SaveLoadInfo *sli) {
  sli->func(sli, apu->ram, offsetof(Apu, pad) + 6 - offsetof(Apu, ram));
  dsp_saveload(apu->dsp, sli);
  spc_saveload(apu->spc, sli);
}

extern uint64_t g_spc_pc_histogram[0x10000];
extern int g_spc_pc_max_seen;

void apu_cycle(Apu* apu) {
  if(apu->cpuCyclesLeft == 0) {
    /* Sample PC right BEFORE running the opcode — so PC reflects the
     * instruction we're about to execute, not the post-opcode PC. */
    g_spc_pc_histogram[apu->spc->pc]++;
    if (apu->spc->pc > g_spc_pc_max_seen) g_spc_pc_max_seen = apu->spc->pc;
    apu->cpuCyclesLeft = spc_runOpcode(apu->spc);
  }
  apu->cpuCyclesLeft--;

  if((apu->cycles & 0x1f) == 0) {
    // every 32 cycles
    apu_drainPortQueue(apu);
    dsp_cycle(apu->dsp);
  }

  // handle timers
  extern uint64_t g_apu_timer0_total_ticks;
  for(int i = 0; i < 3; i++) {
    if(apu->timer[i].cycles == 0) {
      apu->timer[i].cycles = i == 2 ? 16 : 128;
      if(apu->timer[i].enabled) {
        apu->timer[i].divider++;
        if(apu->timer[i].divider == apu->timer[i].target) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter++;
          apu->timer[i].counter &= 0xf;
          if (i == 0) g_apu_timer0_total_ticks++;
        }
      }
    }
    apu->timer[i].cycles--;
  }

  apu->cycles++;
}

uint8_t apu_cpuRead(Apu* apu, uint16_t adr) {
  switch(adr) {
    case 0xf0:
    case 0xf1:
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      return 0;
    }
    case 0xf2: {
      return apu->dspAdr;
    }
    case 0xf3: {
      return dsp_read(apu->dsp, apu->dspAdr & 0x7f);
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      uint8_t v = apu->inPorts[adr - 0xf4];
      audio_trace_on_spc_port_read((uint8_t)(adr - 0xf4), v);
      return v;
    }
    case 0xf8:
    case 0xf9: {
      return apu->inPorts[adr - 0xf4];
    }
    case 0xfd:
    case 0xfe:
    case 0xff: {
      uint8_t ret = apu->timer[adr - 0xfd].counter;
      apu->timer[adr - 0xfd].counter = 0;
      return ret;
    }
  }
  if(apu->romReadable && adr >= 0xffc0) {
    return bootRom[adr - 0xffc0];
  }
  return apu->ram[adr];
}

/* Diagnostic counters: track SPC writes to specific addresses so we
 * can see whether the engine is touching outPorts at all. */
uint64_t g_spc_write_counts[0x100] = {0};

/* SPC PC histogram. Sampled once per apu_cycle that starts a new
 * opcode. Lets us answer "which PCs does the SPC spend time in?" */
uint64_t g_spc_pc_histogram[0x10000] = {0};
int g_spc_pc_max_seen = 0;
/* Per-value count for outPorts $F4-$F7. Index = (port_idx * 256) + val. */
uint64_t g_spc_outport_value_counts[4 * 256] = {0};
/* Last 32 outPort writes as a ring buffer: [adr, val] pairs. */
typedef struct { uint8_t adr; uint8_t val; } SpcWriteRec;
SpcWriteRec g_spc_recent_outport_writes[32];
int g_spc_recent_outport_idx = 0;

void apu_cpuWrite(Apu* apu, uint16_t adr, uint8_t val) {
  if (adr < 0x100) g_spc_write_counts[adr]++;
  if (adr >= 0xF4 && adr <= 0xF7) {
    int port = adr - 0xF4;
    g_spc_outport_value_counts[port * 256 + val]++;
    int i = g_spc_recent_outport_idx++ & 31;
    g_spc_recent_outport_writes[i].adr = (uint8_t)adr;
    g_spc_recent_outport_writes[i].val = val;
  }
  switch(adr) {
    case 0xf0: {
      break; // test register
    }
    case 0xf1: {
      for(int i = 0; i < 3; i++) {
        if(!apu->timer[i].enabled && (val & (1 << i))) {
          apu->timer[i].divider = 0;
          apu->timer[i].counter = 0;
        }
        apu->timer[i].enabled = val & (1 << i);
      }
      if(val & 0x10) {
        apu->inPorts[0] = 0;
        apu->inPorts[1] = 0;
      }
      if(val & 0x20) {
        apu->inPorts[2] = 0;
        apu->inPorts[3] = 0;
      }
      apu->romReadable = val & 0x80;
      break;
    }
    case 0xf2: {
      apu->dspAdr = val;
      break;
    }
    case 0xf3: {
      if(apu->dspAdr < 0x80) dsp_write(apu->dsp, apu->dspAdr, val);
      break;
    }
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xf7: {
      audio_trace_on_spc_port_write((uint8_t)(adr - 0xf4), val);
      apu->outPorts[adr - 0xf4] = val;
      break;
    }
    case 0xf8:
    case 0xf9: {
      apu->inPorts[adr - 0xf4] = val;
      break;
    }
    case 0xfa:
    case 0xfb:
    case 0xfc: {
      apu->timer[adr - 0xfa].target = val;
      break;
    }
  }
  apu->ram[adr] = val;
}
