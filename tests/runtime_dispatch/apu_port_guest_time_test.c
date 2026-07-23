#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apu.h"
#include "dsp_shadow.h"

uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;

static unsigned applied_count;

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
  (void)port;
  (void)value;
  applied_count++;
}
void audio_trace_on_spc_port_read(uint8_t port, uint8_t value) {
  (void)port;
  (void)value;
}
void audio_trace_on_spc_port_write(uint8_t port, uint8_t value) {
  (void)port;
  (void)value;
}
void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill) {
  (void)left;
  (void)right;
  (void)dropped;
  (void)ring_fill;
}
void audio_trace_on_reg_write(uint8_t address, uint8_t value) {
  (void)address;
  (void)value;
}
void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after) {
  (void)read_index;
  (void)count;
  (void)available_after;
}
void audio_trace_on_faithful_div(double divergence) { (void)divergence; }
void audio_trace_on_brr_compare(uint16_t block, uint8_t header, uint8_t sample,
                                int canon, int reference, int old, int older) {
  (void)block; (void)header; (void)sample; (void)canon;
  (void)reference; (void)old; (void)older;
}
void audio_trace_on_echo_div(double divergence) { (void)divergence; }
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *out_left, int *out_right) {
  (void)shadow;
  (void)dsp;
  *out_left = canonical_left;
  *out_right = canonical_right;
}

static int check(int condition, const char *message) {
  if (!condition)
    fprintf(stderr, "FAIL: %s\n", message);
  return condition ? 0 : 1;
}

int main(void) {
  int failures = 0;
  Apu *apu = apu_init();

  /* SMW writes a nonzero command in one NMI and clears it in the next.
   * Host callback phase must not shorten that emulated frame. */
  failures += check(apu_schedulePortWrite(apu, 0, 0x1f, 1000),
                    "queue first frame command");
  failures += check(apu_schedulePortWrite(apu, 0, 0x00, 1000 + 17088),
                    "queue next frame clear");
  apu_cycle(apu);
  failures += check(apu->inPorts[0] == 0x1f && applied_count == 1,
                    "command lands at its first due cycle");
  for (int i = 1; i < 17088; i++)
    apu_cycle(apu);
  failures += check(apu->inPorts[0] == 0x1f && applied_count == 1,
                    "command remains visible for one emulated frame");
  apu_cycle(apu);
  failures += check(apu->inPorts[0] == 0x00 && applied_count == 2,
                    "clear lands exactly one guest frame later");

  /* A word write shares one guest timestamp. Preserve high-byte then low-byte
   * insertion order without inventing time between them. */
  apu_clearPortQueue(apu);
  failures += check(apu_schedulePortWrite(apu, 1, 0x44, 20000),
                    "queue word high byte");
  failures += check(apu_schedulePortWrite(apu, 0, 0x55, 20000),
                    "queue word low byte");
  failures += check(apu->portQueue[apu->portQHead].target_cycle ==
                        apu->portQueue[apu->portQHead + 1].target_cycle &&
                    apu->portQueue[apu->portQHead].port == 1 &&
                    apu->portQueue[apu->portQHead + 1].port == 0,
                    "same-cycle word bytes retain bus order");

  /* If the callback has moved the SPC beyond the old mapping, rebase once and
   * preserve later guest deltas from the new live point. */
  apu_clearPortQueue(apu);
  apu->portClock = 50000;
  failures += check(apu_schedulePortWrite(apu, 2, 0x80, 30000),
                    "queue callback-ahead anchor");
  apu->portQHead = apu->portQTail;
  apu->portClock = 51000;
  failures += check(apu_schedulePortWrite(apu, 2, 0x23, 30100),
                    "rebase callback-ahead event");
  failures += check(apu_schedulePortWrite(apu, 2, 0x00, 30200),
                    "queue event after rebase");
  ApuPortWrite *rebased = &apu->portQueue[(apu->portQTail - 2) &
                                          (APU_PORT_QUEUE_LEN - 1)];
  ApuPortWrite *following = &apu->portQueue[(apu->portQTail - 1) &
                                            (APU_PORT_QUEUE_LEN - 1)];
  failures += check(rebased->target_cycle == 51000 &&
                    following->target_cycle == 51100,
                    "callback-ahead rebase preserves the next guest delta");

  failures += check(apu_runToGuestCycle(apu, 30200, 200) &&
                    apu->portClock >= 51100 &&
                    apu->inPorts[2] == 0x00,
                    "guest-clock sync advances SPC through all due bus events");

  /* Saturation must backpressure the producer rather than prematurely expose
   * an old command or discard a new one. */
  apu_clearPortQueue(apu);
  for (unsigned i = 0; i < APU_PORT_QUEUE_LEN; i++)
    failures += check(apu_schedulePortWrite(apu, i & 3, (uint8_t)i, 40000 + i),
                      "fill bounded queue");
  uint32_t head_before = apu->portQHead;
  failures += check(!apu_schedulePortWrite(apu, 0, 0xaa, 50000) &&
                    apu->portQHead == head_before &&
                    apu_portQueueDepth(apu) == APU_PORT_QUEUE_LEN,
                    "full queue applies backpressure without mutation");

  /* Declared live HLE uploads synchronize with the running driver instead of
   * erasing its port state at the transfer boundary. */
  apu_clearPortQueue(apu);
  memset(apu->inPorts, 0, sizeof(apu->inPorts));
  memset(apu->outPorts, 0, sizeof(apu->outPorts));
  apu->outPorts[0] = 0xaa;
  apu->outPorts[1] = 0xbb;
  failures += check(apu_waitForTransferReady(apu, 1, 0xff, 0),
                    "live upload accepts the driver-ready handshake");
  apu->outPorts[0] = 0;
  failures += check(!apu_waitForTransferReady(apu, 1, 0xff, 0) &&
                    apu->inPorts[1] == 0xff,
                    "live upload reasserts a request while awaiting ready");
  apu->outPorts[0] = 0xcc;
  failures += check(apu_finishHleTransfer(apu, 0x1234, 0) &&
                    apu->inPorts[0] == 0 && apu->inPorts[1] == 0 &&
                    apu->inPorts[2] == 0 && apu->inPorts[3] == 0,
                    "live upload completes CC acknowledgement and clears ports");

  apu->dsp->sampleRead = 100;
  apu->dsp->sampleWrite = 419;
  failures += check(dsp_trimSamples(apu->dsp, 64) == 255 &&
                    apu->dsp->sampleRead == 355 &&
                    apu->dsp->sampleWrite == 419,
                    "fast-forward recovery retains only the requested newest PCM");

  apu_free(apu);
  if (failures)
    return 1;
  puts("apu_port_guest_time_test: PASS");
  return 0;
}
