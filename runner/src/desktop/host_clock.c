/* host_clock.c — see host_clock.h. */
#include "host_clock.h"

#include <math.h>

void snes_host_clock_reset(SnesHostClock *c, double now,
                           double simulation_hz, double presentation_hz) {
  *c = (SnesHostClock){
      .next_simulation = now,
      .next_presentation = now,
      .simulation_hz = isfinite(simulation_hz) && simulation_hz > 0
                           ? simulation_hz : SNES_HOST_NTSC_HZ,
      .presentation_hz = isfinite(presentation_hz) && presentation_hz > 0
                             ? presentation_hz : 60};
}

bool snes_host_clock_simulation_due(const SnesHostClock *c, double now) {
  return now >= c->next_simulation;
}

void snes_host_clock_simulation_done(SnesHostClock *c, double now,
                                     bool preserve_debt, double elapsed_periods) {
  double deadline = c->next_simulation + elapsed_periods / c->simulation_hz;
  /* Keep ordinary sub-frame scheduling jitter on the original phase. A
   * loader may preserve its backlog so its non-interactive frames refill the
   * guest-driven audio queue; all ordinary gameplay drops stale work. */
  c->next_simulation = preserve_debt || now <= deadline
      ? deadline : now + 1.0 / c->simulation_hz;
  ++c->simulation_frames;
}

bool snes_host_clock_presentation_due(const SnesHostClock *c, double now) {
  return now >= c->next_presentation;
}

void snes_host_clock_presentation_done(SnesHostClock *c, double now) {
  double period = 1.0 / c->presentation_hz;
  double overdue = fmax(0, now - c->next_presentation);
  uint64_t missed = (uint64_t)floor(overdue / period);
  c->missed_presentations += missed;
  c->next_presentation += (double)(missed + 1) * period;
  ++c->presentations;
}

double snes_host_clock_alpha(const SnesHostClock *c, double now) {
  return fmax(0, fmin(1, 1 - (c->next_simulation - now) * c->simulation_hz));
}

double snes_host_clock_next_deadline(const SnesHostClock *c) {
  return fmin(c->next_simulation, c->next_presentation);
}

bool snes_host_valid_fps(unsigned fps) {
  return fps == 0 || fps == 60 || fps == 90 || fps == 120 || fps == 144 ||
         fps == 165 || fps == 240 || fps == 360;
}

double snes_host_presentation_hz(unsigned fps, double display_refresh) {
  if (!snes_host_valid_fps(fps)) fps = 0;
  if (fps) return fps;
  if (!isfinite(display_refresh) || display_refresh < 1) return 60;
  return fmin(display_refresh, 360);
}
