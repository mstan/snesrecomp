/*
 * snes9x_bridge.h — extern "C" surface for the snes9x oracle backend.
 *
 * The bridge exports a single snes_oracle_backend_t instance
 * (g_snes9x_backend) consumed by emu_oracle_cmds.c. The C function
 * entry points below exist so the struct's function pointers can be
 * initialized from C++ — callers should normally go through
 * g_active_backend, not these names directly.
 */
#ifndef SNES9X_BRIDGE_H
#define SNES9X_BRIDGE_H

#include "snes_oracle_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

int     snes9x_bridge_init(const char *rom_path);
void    snes9x_bridge_run_frame(uint16_t joypad1, uint16_t joypad2);
void    snes9x_bridge_shutdown(void);
int     snes9x_bridge_is_loaded(void);
void    snes9x_bridge_get_wram(uint8_t *out);          /* 128 KB */
uint8_t snes9x_bridge_cpu_read(uint32_t addr24);
void    snes9x_bridge_get_cpu_regs(SnesCpuRegs *out);

/* Report bytes that changed in the MOST RECENT retro_run(). Bounded
 * to [lo, hi] (inclusive). Writes into caller-provided parallel arrays
 * up to out_caps entries. Returns the number of entries written. */
int     snes9x_bridge_get_wram_delta(uint32_t lo, uint32_t hi,
                                     uint32_t *out_addrs,
                                     uint8_t *out_before,
                                     uint8_t *out_after,
                                     int out_caps);

/* Tier-1-equivalent WRAM write watchpoint. Installs a per-write hook
 * inside snes9x's memory bus (via getset.h::s9x_write_hook). Records
 * (frame, addr, pc24, before, after, bank_source) for every write that
 * hits a watched range. Max 8 ranges, 16384-entry ring. */
int     snes9x_bridge_watch_add(uint32_t lo, uint32_t hi);  /* returns new nranges, or negative on error */
void    snes9x_bridge_watch_clear(void);
int     snes9x_bridge_watch_count(void);
int     snes9x_bridge_watch_get(int i, uint32_t *frame, uint32_t *addr,
                                uint32_t *pc24, uint8_t *before, uint8_t *after,
                                uint8_t *bank_source);

/* Per-instruction trace. Captures (frame, pc24, op, A, X, Y, S, D,
 * DB, P_W, cycles) at every CPU dispatch. Ring of 1M entries
 * (~24 MB). NMI counter ticks separately and persists across
 * insn-trace on/off. */
void     snes9x_bridge_insn_trace_on(void);
void     snes9x_bridge_insn_trace_off(void);
void     snes9x_bridge_insn_trace_reset(void);
uint64_t snes9x_bridge_insn_trace_count(void);
uint64_t snes9x_bridge_nmi_count(void);
int      snes9x_bridge_insn_trace_get(uint64_t i, int32_t *frame,
                                      uint32_t *pc24, uint8_t *op,
                                      uint8_t *db, uint16_t *a, uint16_t *x,
                                      uint16_t *y, uint16_t *s, uint16_t *d,
                                      uint16_t *p_w, int32_t *cycles);

/* Backend instance. Registered into emu_oracle_cmds.c's backend
 * registry via ENABLE_SNES9X_ORACLE. */
extern const snes_oracle_backend_t g_snes9x_backend;

#ifdef __cplusplus
}
#endif

#endif
