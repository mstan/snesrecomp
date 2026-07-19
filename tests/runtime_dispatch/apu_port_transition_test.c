#include <stdio.h>
#include <string.h>

#include "apu.h"

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
  (void)port;
  (void)value;
}

static int check(int condition, const char *message) {
  if (!condition)
    fprintf(stderr, "FAIL: %s\n", message);
  return condition ? 0 : 1;
}

int main(void) {
  Apu apu;
  int failures = 0;

  memset(&apu, 0, sizeof(apu));
  apu_clearPortQueue(&apu);

  /* A transition can queue a fade, music command, and NMI clear before the
   * audio thread advances another sample. Every distinct command must remain
   * visible for at least one SPC poll. */
  apu_schedulePortWrite(&apu, 2, 0x80, 1000);
  apu_schedulePortWrite(&apu, 2, 0x23, 1000);
  apu_schedulePortWrite(&apu, 2, 0x00, 1000);

  apu_applyDuePortWrites(&apu, 1000);
  failures += check(apu.inPorts[2] == 0x80, "fade visible before music");
  apu_noteSpcPortRead(&apu, 2, 0x80);

  apu_applyDuePortWrites(&apu, 1000 + APU_PORT_MIN_DWELL);
  failures += check(apu.inPorts[2] == 0x23, "music visible before clear");
  apu_noteSpcPortRead(&apu, 2, 0x23);

  apu_applyDuePortWrites(&apu, 1000 + 2 * APU_PORT_MIN_DWELL);
  failures += check(apu.inPorts[2] == 0x00, "clear follows music observation");

  if (failures)
    return 1;
  puts("apu_port_transition_test: PASS");
  return 0;
}
