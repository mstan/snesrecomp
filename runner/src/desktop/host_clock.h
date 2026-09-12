#pragma once

/*
 * host_clock.h — the desktop host's simulation/presentation pacing clock.
 *
 * Two independent deadlines: when the next guest frame is due, and when the
 * next presented frame is due. They coincide for an ordinary host (present
 * every simulated frame). A host with a custom presenter may present at the
 * display's refresh rate while simulating at the guest's, interpolating
 * between simulated frames; `alpha` is the interpolation weight.
 *
 * Debt policy: an ordinary slow frame drops its wall-time debt (the next
 * deadline is re-anchored to now) so the guest never bursts forward to catch
 * up. A game may ask to PRESERVE the debt for phases where bursting is the
 * right thing -- Super Metroid's door transitions, whose non-interactive
 * frames refill the guest-driven audio queue -- through the host descriptor's
 * keep_pacing_debt hook.
 *
 * Moved up from SuperMetroidRecomp's sm_video.c, where it was called SmClock
 * and hardwired to that title's frame rate. Nothing in it was Super Metroid.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NTSC SNES field rate: 21.477272 MHz / (262 * 1364). */
#define SNES_HOST_NTSC_HZ 60.098811862

typedef struct SnesHostClock {
  double next_simulation, next_presentation;
  double simulation_hz, presentation_hz;
  uint64_t simulation_frames, presentations, missed_presentations;
} SnesHostClock;

void   snes_host_clock_reset(SnesHostClock *c, double now,
                             double simulation_hz, double presentation_hz);
bool   snes_host_clock_simulation_due(const SnesHostClock *c, double now);
/* `elapsed_periods` is how many guest frame periods the simulated frame
 * actually spanned (RtlLastFramePeriods()); a lag frame spans more than one. */
void   snes_host_clock_simulation_done(SnesHostClock *c, double now,
                                       bool preserve_debt, double elapsed_periods);
bool   snes_host_clock_presentation_due(const SnesHostClock *c, double now);
void   snes_host_clock_presentation_done(SnesHostClock *c, double now);
double snes_host_clock_alpha(const SnesHostClock *c, double now);
double snes_host_clock_next_deadline(const SnesHostClock *c);

/* Which presentation rate to run at: an explicit fps if it is one the clock
 * supports, else the display's refresh, clamped to a sane range. 0 = display. */
bool   snes_host_valid_fps(unsigned fps);
double snes_host_presentation_hz(unsigned fps, double display_refresh);

#ifdef __cplusplus
}
#endif
