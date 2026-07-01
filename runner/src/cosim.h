/*
 * cosim.h -- co-simulation engine hooks (SNES_COSIM.md). DEV/DIAGNOSTICS ONLY.
 * All no-ops unless built with SNES_COSIM. Called from the recomp runtime
 * (RtlRunFrame) and from the snes-cosim-ref driver.
 */
#ifndef COSIM_H
#define COSIM_H

#ifdef SNES_COSIM
void cosim_init(void);    /* listen + accept the coordinator (blocks until connected) */
void cosim_frame(void);   /* call at every completed guest frame */
void cosim_prime(void);   /* optional: park once before frame 1 */
#else
#define cosim_init()   ((void)0)
#define cosim_frame()  ((void)0)
#define cosim_prime()  ((void)0)
#endif

#endif /* COSIM_H */
