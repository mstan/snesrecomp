/*
 * cosim.h -- co-simulation engine hooks (SNES_COSIM.md). DEV/DIAGNOSTICS ONLY.
 * All no-ops unless built with SNES_COSIM. Called from the recomp runtime
 * (RtlRunFrame) and from the snes-cosim-ref driver.
 */
#ifndef COSIM_H
#define COSIM_H

#ifdef SNES_COSIM
#include <stdint.h>
void cosim_init(void);    /* listen + accept the coordinator (blocks until connected) */
void cosim_frame(void);   /* call at every completed guest frame */
void cosim_insn(uint32_t pc24);  /* per interpreted opcode; instruction-granular lockstep
                                  * once the guest reaches SNES_COSIM_SYNC_PC (low-16 match),
                                  * checkpointing every SNES_COSIM_ISTRIDE opcodes. Disabled
                                  * unless SNES_COSIM_SYNC_PC is set (then cosim_frame no-ops). */
void cosim_prime(void);   /* optional: park once before frame 1 */
#else
#define cosim_init()   ((void)0)
#define cosim_frame()  ((void)0)
#define cosim_insn(pc) ((void)0)
#define cosim_prime()  ((void)0)
#endif

#endif /* COSIM_H */
