#include <stdio.h>
#include <string.h>

#include "apu.h"

static int applied_count;
static uint8_t applied_port;
static uint8_t applied_value;
uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
  applied_count++;
  applied_port = port;
  applied_value = value;
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

  apu_writePortNow(&apu, 2, 0x80);
  failures += check(apu.inPorts[2] == 0x80,
                    "CPU port write is visible in the current APU cycle");
  failures += check(applied_count == 1 && applied_port == 2 &&
                    applied_value == 0x80,
                    "CPU port write is traced when it becomes visible");

  apu_writePortNow(&apu, 2, 0x23);
  failures += check(apu.inPorts[2] == 0x23,
                    "a later command replaces the port immediately");

  apu_writePortNow(&apu, 6, 0x00);
  failures += check(apu.inPorts[2] == 0x00 && applied_port == 2 &&
                    applied_value == 0x00,
                    "port index is masked and clear is immediately visible");

  memset(&apu, 0, sizeof(apu));
  apu.outPorts[0] = 0xaa;
  apu.outPorts[1] = 0xbb;
  failures += check(apu_waitForTransferReady(&apu, 1, 0xff, 0),
                    "HLE upload waits for the SPC transfer-ready handshake");

  apu.outPorts[0] = 0;
  failures += check(!apu_waitForTransferReady(&apu, 1, 0xff, 0),
                    "HLE upload rejects a missing transfer-ready handshake");
  failures += check(apu.inPorts[1] == 0xff,
                    "HLE upload reasserts a transfer request cleared during startup");

  apu.outPorts[0] = 0xcc;
  failures += check(apu_finishHleTransfer(&apu, 0x1234, 0),
                    "HLE upload waits for the SPC terminator acknowledgement");
  failures += check(apu.inPorts[0] == 0 && apu.inPorts[1] == 0 &&
                    apu.inPorts[2] == 0 && apu.inPorts[3] == 0,
                    "HLE upload clears CPU ports after acknowledgement");

  if (failures)
    return 1;
  puts("apu_port_transition_test: PASS");
  return 0;
}
