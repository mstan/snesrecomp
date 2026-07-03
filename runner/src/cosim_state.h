/*
 * cosim_state.h -- full guest-architectural-state canonical hash for the
 * SNES differential co-simulation (SNES_COSIM.md). ONE module compiled into
 * BOTH the recomp `snes-cosim` build and the `snes-cosim-ref` (interp816)
 * build, so the two sides hash IDENTICALLY by construction.
 *
 * DEV/DIAGNOSTICS ONLY. Everything here is `#ifdef SNES_COSIM`; it is never
 * compiled into a shipping Production config (SNES_COSIM.md "Production
 * discipline"). Zero bytes in released exes.
 *
 * Build selection:
 *   SNES_COSIM                    -> compiled at all
 *   SNES_COSIM && SNES_COSIM_REF  -> B side: read the Interp816 + its Snes
 *   SNES_COSIM && !SNES_COSIM_REF -> A side: read g_cpu + g_snes + g_ram
 */
#ifndef COSIM_STATE_H
#define COSIM_STATE_H
#ifdef SNES_COSIM

#include <stdint.h>
#include <stddef.h>

/* Sub-hash buckets. The FIRST COSIM_SUB_COMPARED are guest-architectural
 * state folded into `combined` (the compared quantity). The rest are
 * DIAGNOSTIC-ONLY: DSP/SPC are subsets of APU exposed for finer localization;
 * PACE is recomp-internal synthetic-pacing bookkeeping the ref has no
 * equivalent of (reported for provenance, never compared — its EFFECT shows
 * in APU/DSP which ARE compared). */
typedef enum {
    COSIM_SUB_CPU = 0,  /* canonical 65816 regs: A,X,Y,S,D,DB,PB,P,E (no PC) */
    COSIM_SUB_RAM,      /* 128K WRAM */
    COSIM_SUB_APU,      /* apu_saveload: SPC RAM + DSP + SPC700 (subsumes DSP+SPC) */
    COSIM_SUB_PPU,
    COSIM_SUB_DMA,
    COSIM_SUB_CART,
    COSIM_SUB_SIO,      /* snes blob hPos..divideResult + ramAdr */
    /* --- diagnostic-only below; NOT in `combined` --- */
    COSIM_SUB_DSP,      /* dsp_saveload alone */
    COSIM_SUB_SPC,      /* spc_saveload alone */
    COSIM_SUB_PACE,     /* recomp-only synthetic pacing globals (0 on ref) */
    COSIM_SUB_COUNT
} CosimSub;

#define COSIM_SUB_COMPARED  (COSIM_SUB_SIO + 1)   /* first N folded into combined */

extern const char *const cosim_sub_names[COSIM_SUB_COUNT];

typedef struct {
    uint64_t sub[COSIM_SUB_COUNT];  /* per-subsystem FNV-1a of current state */
    uint64_t combined;              /* fold of sub[0..COSIM_SUB_COMPARED) */
    /* Reported-only (never in `combined`/chain): cycle-model + labels. */
    uint64_t cycles;                /* bus cycles (recomp g_cpu.cycles / ref sum) */
    uint64_t master_cycles;         /* THE ruler value at snapshot time */
    uint32_t last_leader_pc;        /* human label only (ref pc24 / recomp leader) */
} CosimSnapshot;

/* Fill a full snapshot of the CURRENT guest-architectural state. MUST be
 * side-effect-free (pure reads) — reading parked state must not perturb the
 * next step. */
void cosim_state_snapshot(CosimSnapshot *out);

/* The shared alignment clock (SNES master clocks, 21.47727 MHz). Recomp:
 * g_cpu.master_cycles (Axis-2, already accumulated per-block by generated C).
 * Ref: the driver's region-weighted accumulator (same snes_cycles model). */
uint64_t cosim_state_ruler(void);

/* Gate-3 fault injection. Returns 0 on success, non-zero on bad arg. */
int cosim_state_inject_ram(uint32_t addr, uint8_t val);
int cosim_state_inject_reg(const char *reg, uint32_t val);

/* Field dumps for the coordinator's field-diff (space-separated key=hexval). */
void cosim_state_dump_cpu(char *buf, size_t n);
void cosim_state_dump_dev(char *buf, size_t n);

/* Write the current PPU render-buffer contents (last draw_ppu_frame) as a
 * 24-bit BMP. Read-only (no re-render) so parked state stays unperturbed.
 * Returns 0 on success. */
int cosim_state_dump_fb(const char *path);

#endif /* SNES_COSIM */
#endif /* COSIM_STATE_H */
