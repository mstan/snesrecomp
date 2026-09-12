/*
 * host_clock_test.c — the desktop host's pacing clock (runner/src/desktop/
 * host_clock.c), checked in isolation.
 *
 * These cases came up from SuperMetroidRecomp with the clock itself. Each one
 * is a bug that shipped: a lag frame that made the next N frames burst, a
 * paused window whose wall-time debt was repaid as a sprint, a loader whose
 * 18 hardware periods were counted a second time as 17 extra iterations.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "host_clock.h"

int main(void) {
  const double sim = SNES_HOST_NTSC_HZ;
  const unsigned rates[] = {60, 90, 120, 144, 165, 240, 360};
  for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
    SnesHostClock clock;
    snes_host_clock_reset(&clock, 0, sim, rates[r]);
    for (int ms = 0; ms <= 10000; ++ms) {
      double now = ms / 1000.0;
      while (snes_host_clock_simulation_due(&clock, now))
        snes_host_clock_simulation_done(&clock, now, false, 1);
      if (snes_host_clock_presentation_due(&clock, now))
        snes_host_clock_presentation_done(&clock, now);
      double alpha = snes_host_clock_alpha(&clock, now);
      assert(alpha >= 0 && alpha <= 1);
    }
    /* Ten seconds of NTSC frames, whatever the presentation rate. */
    assert(clock.simulation_frames == 601);
    assert(clock.presentations >= rates[r] * 10 - 1 &&
           clock.presentations <= rates[r] * 10 + 1);
  }

  /* Realtime play must discard stalled wall-time debt: five seconds without
   * a simulated frame is one frame, not three hundred. */
  SnesHostClock stalled;
  snes_host_clock_reset(&stalled, 0, sim, 144);
  snes_host_clock_presentation_done(&stalled, 5);
  assert(stalled.simulation_frames == 0 && snes_host_clock_simulation_due(&stalled, 5));
  while (snes_host_clock_simulation_due(&stalled, 5))
    snes_host_clock_simulation_done(&stalled, 5, false, 1);
  assert(stalled.simulation_frames == 1);

  /* A title may ask to PRESERVE the debt (a loading phase that refills a
   * guest-driven audio queue) -- then the backlog is repaid in full. */
  SnesHostClock loading;
  snes_host_clock_reset(&loading, 0, sim, 144);
  while (snes_host_clock_simulation_due(&loading, 5))
    snes_host_clock_simulation_done(&loading, 5, true, 1);
  assert(loading.simulation_frames == 301);

  /* A frame that spanned 18 hardware periods already generated their audio.
   * Waiting those periods must not trigger 17 extra game iterations. */
  SnesHostClock extended;
  snes_host_clock_reset(&extended, 0, sim, 165);
  snes_host_clock_simulation_done(&extended, 0.23, true, 18);
  assert(fabs(extended.next_simulation - 18 / sim) < 1e-9);
  assert(!snes_host_clock_simulation_due(&extended, 0.29));
  snes_host_clock_simulation_done(&extended, 18 / sim, false, 1);
  assert(fabs(extended.next_simulation - 19 / sim) < 1e-9);

  /* A PAL title passes its own rate; nothing in the clock is NTSC. */
  SnesHostClock pal;
  snes_host_clock_reset(&pal, 0, 50.0, 50.0);
  for (int ms = 0; ms <= 1000; ++ms) {
    double now = ms / 1000.0;
    while (snes_host_clock_simulation_due(&pal, now))
      snes_host_clock_simulation_done(&pal, now, false, 1);
  }
  assert(pal.simulation_frames >= 50 && pal.simulation_frames <= 51); /* 1.0 vs 50 x 0.02 in floating point */

  /* Presentation-rate policy: an explicit supported fps wins, otherwise the
   * display's refresh, clamped; garbage falls back to 60. */
  assert(snes_host_valid_fps(0) && snes_host_valid_fps(144) && !snes_host_valid_fps(100));
  assert(snes_host_presentation_hz(0, 165) == 165);
  assert(snes_host_presentation_hz(0, 1000) == 360);
  assert(snes_host_presentation_hz(0, NAN) == 60);
  assert(snes_host_presentation_hz(120, 165) == 120);
  assert(snes_host_presentation_hz(100, 165) == 165);

  puts("host_clock: all checks passed");
  return 0;
}
