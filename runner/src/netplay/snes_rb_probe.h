/* See snes_rb_probe.c. Called at the end of every guest frame; a no-op
 * unless SNESRECOMP_RB_PROBE is set. */
#ifndef SNES_RB_PROBE_H
#define SNES_RB_PROBE_H
#include <stdint.h>
void snes_rb_probe_after_frame(uint32_t inputs);
/* True while the probe is armed; the runner holds an armed run to netplay's
 * APU-ownership rules so the probe measures replay, not host clock drift. */
int snes_rb_probe_armed(void);
#endif
