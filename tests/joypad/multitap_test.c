/*
 * Super Multitap protocol.
 *
 * The behaviour pinned here comes from the SNESdev wiki "Multitap" page and
 * the Super Famicom development wiki "Controllers" page, not from reading
 * another emulator. The two facts everything else hangs off:
 *
 *   - $4201 bit 6 drives port 1's IOBit and bit 7 drives port 2's. A tap
 *     reports its first pad pair when its IOBit is 1 and its second pair
 *     when it is 0.
 *   - A tap reports 17 bits per pad; the 17th is 1 when that tap slot has a
 *     controller in it and 0 when it is empty.
 *
 * The last test walks the exact five-player read sequence the wiki gives, in
 * order, because that sequence is the thing a real game does and it is the
 * only one that exercises the automatic read and the manual read together.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "joypad.h"
#include "snes.h"

static int g_failures;

static void check(int ok, const char *what)
{
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what);
    g_failures++;
  }
}

/* Button bit order is B,Y,Select,Start,Up,Down,Left,Right,A,X,L,R. */
#define BTN_B      (1u << 0)
#define BTN_START  (1u << 3)
#define BTN_UP     (1u << 4)
#define BTN_RIGHT  (1u << 7)
#define BTN_A      (1u << 8)
#define BTN_R      (1u << 11)

static void strobe(Snes *s)
{
  joypad_write_strobe(s, 1);
  joypad_write_strobe(s, 0);
}

/* Read `count` serial bits from a port, returning the bits of one data line
 * in the order the hardware shifts them out (first bit -> bit 0). */
static uint32_t shift_line(Snes *s, unsigned port, int line, int count)
{
  uint32_t out = 0;
  int i;
  for (i = 0; i < count; i++) {
    uint8_t v = joypad_read_port(s, port);
    out |= (uint32_t)((v >> line) & 1u) << i;
  }
  return out;
}

static void reset_all(Snes *s, int tap0, int tap1)
{
  memset(s, 0, sizeof(*s));
  joypad_set_multitap(0, tap0);
  joypad_set_multitap(1, tap1);
  joypad_reset_state();
  /* IOBit high on both ports = first pad pair, the normal resting state. */
  joypad_write_iobit(s, 0xc0);
}

/* ── no tap: everything must behave exactly as it did before ─────────── */

static void test_no_tap_unchanged(void)
{
  Snes s;
  int bit;

  reset_all(&s, 0, 0);
  s.input1_currentState = BTN_B | BTN_START | BTN_A | BTN_R;
  strobe(&s);
  for (bit = 0; bit < 16; bit++) {
    uint8_t v = joypad_read_port(&s, 0);
    uint8_t expect = (bit == 0 || bit == 3 || bit == 8 || bit == 11);
    check((v & 1u) == expect, "no-tap port 1 serial bit order");
    check((v & 2u) == 0, "no-tap port leaves Data2 clear");
  }
  check((joypad_read_port(&s, 0) & 1u) == 1,
        "no-tap reads past bit 15 report 1");

  /* Automatic read registers still answer from live pad state with no tap,
   * so a game that never enables the automatic-read handshake keeps working. */
  reset_all(&s, 0, 0);
  s.input1_currentState = BTN_A;
  check(joypad_auto_read_reg_addr(&s, 0x4218) == 0x80,
        "no-tap $4218 matches the pre-multitap value");
  check(joypad_auto_read_reg_addr(&s, 0x421c) == 0x00,
        "no-tap $421C (Data2) reads 0");
}

/* ── detection ───────────────────────────────────────────────────────── */

static void test_detection(void)
{
  Snes s;
  uint32_t high, low;

  /* Wiki sequence: strobe high -> eight Data2 reads must be $FF; strobe low
   * -> eight Data2 reads must not be $FF. */
  reset_all(&s, 0, 1);
  joypad_write_strobe(&s, 1);
  high = shift_line(&s, 1, 1, 8);
  joypad_write_strobe(&s, 0);
  low = shift_line(&s, 1, 1, 8);
  check(high == 0xffu, "tap reports $FF on Data2 while the strobe is high");
  check(low != 0xffu, "tap reports non-$FF on Data2 once the strobe clears");

  /* A port with no tap must fail the same test, or every session would
   * think it had one. */
  reset_all(&s, 0, 0);
  joypad_write_strobe(&s, 1);
  high = shift_line(&s, 1, 1, 8);
  joypad_write_strobe(&s, 0);
  check(high != 0xffu, "a port with no tap never reports $FF on Data2");
}

/* ── bank select via IOBit ───────────────────────────────────────────── */

static void test_bank_select(void)
{
  Snes s;
  uint32_t d0, d1;

  /* Tap on port 2 => seats 1..4. */
  reset_all(&s, 0, 1);
  check(joypad_player_count() == 5, "one tap reaches five seats");
  joypad_set_pad(1, BTN_B);
  joypad_set_pad(2, BTN_START);
  joypad_set_pad(3, BTN_UP);
  joypad_set_pad(4, BTN_RIGHT);

  joypad_write_iobit(&s, 0xc0);          /* port 2 IOBit = 1 -> first pair */
  strobe(&s);
  d0 = shift_line(&s, 1, 0, 16);
  check(d0 == BTN_B, "IOBit 1 puts seat 1 on Data1");

  strobe(&s);
  d1 = shift_line(&s, 1, 1, 16);
  check(d1 == BTN_START, "IOBit 1 puts seat 2 on Data2");

  joypad_write_iobit(&s, 0x40);          /* port 2 IOBit = 0 -> second pair */
  strobe(&s);
  d0 = shift_line(&s, 1, 0, 16);
  check(d0 == BTN_UP, "IOBit 0 puts seat 3 on Data1");

  strobe(&s);
  d1 = shift_line(&s, 1, 1, 16);
  check(d1 == BTN_RIGHT, "IOBit 0 puts seat 4 on Data2");

  /* Seat 0 is the lone controller on port 1 and ignores IOBit entirely. */
  s.input1_currentState = BTN_A;
  joypad_set_pad(0, BTN_A);
  strobe(&s);
  d0 = shift_line(&s, 0, 0, 16);
  check(d0 == BTN_A, "seat 0 stays on port 1 regardless of IOBit");
}

/* ── the 17th bit ────────────────────────────────────────────────────── */

static void test_connection_bit(void)
{
  Snes s;
  int i;

  reset_all(&s, 0, 1);
  joypad_set_connected(2, 0);            /* seat 2 tap slot left empty */

  joypad_write_iobit(&s, 0xc0);
  strobe(&s);
  for (i = 0; i < 16; i++)
    joypad_read_port(&s, 1);
  {
    uint8_t v = joypad_read_port(&s, 1);
    check((v & 1u) == 1, "17th bit is 1 for an occupied tap slot");
    check((v & 2u) == 0, "17th bit is 0 for an empty tap slot");
  }
  {
    uint8_t v = joypad_read_port(&s, 1);
    check((v & 3u) == 3, "reads past the 17th bit report 1s");
  }
}

/* ── independent per-bank shift counters ─────────────────────────────── */

static void test_bank_counters_independent(void)
{
  Snes s;
  uint32_t first, second;

  reset_all(&s, 0, 1);
  joypad_set_pad(1, 0x0fffu);            /* seat 1: everything held */
  joypad_set_pad(3, 0x0fffu);            /* seat 3: everything held */

  joypad_write_iobit(&s, 0xc0);
  strobe(&s);
  first = shift_line(&s, 1, 0, 8);       /* eight bits of the first bank */
  check(first == 0xffu, "first bank shifted eight bits");

  /* Switching banks must not disturb the bank we were half way through. */
  joypad_write_iobit(&s, 0x40);
  second = shift_line(&s, 1, 0, 16);
  check(second == 0x0fffu, "second bank starts from its own bit 0");

  joypad_write_iobit(&s, 0xc0);
  first = shift_line(&s, 1, 0, 8);
  check(first == 0x0fu,
        "first bank resumes where it left off (bits 8..15)");
}

/* ── automatic read + the documented five-player sequence ────────────── */

static void test_auto_read_mapping(void)
{
  Snes s;

  reset_all(&s, 0, 1);
  joypad_set_pad(0, BTN_A);              /* port 1 controller  -> JOY1 */
  s.input1_currentState = BTN_A;
  joypad_set_pad(1, BTN_B);              /* tap pair 1, Data1  -> JOY2 */
  joypad_set_pad(2, BTN_START);          /* tap pair 1, Data2  -> JOY4 */

  joypad_write_iobit(&s, 0xc0);
  joypad_auto_read(&s);

  /* $4218/9 = port1 Data1, $421A/B = port2 Data1,
   * $421C/D = port1 Data2, $421E/F = port2 Data2. */
  check(joypad_auto_read_reg_addr(&s, 0x4218) == 0x80, "JOY1 = seat 0 (A)");
  check((joypad_auto_read_reg_addr(&s, 0x421b)) == 0x80, "JOY2 = seat 1 (B)");
  check(joypad_auto_read_reg_addr(&s, 0x421c) == 0x00, "JOY3 empty: no tap on port 1");
  check(joypad_auto_read_reg_addr(&s, 0x421d) == 0x00, "JOY3 empty: no tap on port 1");
  check((joypad_auto_read_reg_addr(&s, 0x421f)) == 0x10, "JOY4 = seat 2 (Start)");
}

static void test_five_player_sequence(void)
{
  Snes s;
  uint32_t d0, d1;

  reset_all(&s, 0, 1);
  joypad_set_pad(0, BTN_A);
  s.input1_currentState = BTN_A;
  joypad_set_pad(1, BTN_B);
  joypad_set_pad(2, BTN_START);
  joypad_set_pad(3, BTN_UP);
  joypad_set_pad(4, BTN_RIGHT);
  joypad_set_connected(4, 0);            /* fifth seat empty */

  /* 1. $4201.7 = 1 selects pads 2/3.  2. automatic read captures 1, 2, 3. */
  joypad_write_iobit(&s, 0xc0);
  joypad_auto_read(&s);
  check(joypad_auto_read_reg_addr(&s, 0x4219) == 0x00 &&
        joypad_auto_read_reg_addr(&s, 0x4218) == 0x80,
        "auto read captured seat 0");

  /* 4. one more read of $4017 yields the 17th bit for seats 1 and 2 —
   *    this only works because the automatic read left the counter at 16. */
  {
    uint8_t v = joypad_read_port(&s, 1);
    check((v & 1u) == 1, "seat 1 reports connected after the automatic read");
    check((v & 2u) == 1 << 1, "seat 2 reports connected after the automatic read");
  }

  /* 5. $4201.7 = 0 selects pads 4/5.  6. read their 16-bit reports. */
  joypad_write_iobit(&s, 0x40);
  d0 = shift_line(&s, 1, 0, 16);
  check(d0 == BTN_UP, "seat 3 read after switching banks");

  /* 7. the 17th bit distinguishes the empty fifth seat. */
  {
    uint8_t v = joypad_read_port(&s, 1);
    check((v & 1u) == 1, "seat 3 reports connected");
    check((v & 2u) == 0, "seat 4 reports disconnected");
  }

  /* Re-latch and confirm Data2 of the second bank carries seat 4. */
  joypad_write_iobit(&s, 0x40);
  strobe(&s);
  d1 = shift_line(&s, 1, 1, 16);
  check(d1 == BTN_RIGHT, "seat 4 sits on Data2 of the second bank");

  /* 8. back to $4201.7 = 1 for the next frame. */
  joypad_write_iobit(&s, 0xc0);
  check(joypad_read_iobit() == 0xc0, "RDIO reads back both IOBit lines");
}

/* ── eight players: a tap in each port ───────────────────────────────── */

static void test_eight_players(void)
{
  Snes s;
  int i;

  reset_all(&s, 1, 1);
  check(joypad_player_count() == 8, "two taps reach eight seats");
  for (i = 0; i < 8; i++)
    joypad_set_pad(i, (uint16_t)(1u << i));

  /* Port 1's tap owns seats 0..3 and is selected by $4201 bit 6;
   * port 2's owns 4..7 and is selected by bit 7. */
  joypad_write_iobit(&s, 0xc0);
  strobe(&s);
  check(shift_line(&s, 0, 0, 16) == (1u << 0), "seat 0: port 1 bank 1 Data1");
  strobe(&s);
  check(shift_line(&s, 0, 1, 16) == (1u << 1), "seat 1: port 1 bank 1 Data2");
  strobe(&s);
  check(shift_line(&s, 1, 0, 16) == (1u << 4), "seat 4: port 2 bank 1 Data1");
  strobe(&s);
  check(shift_line(&s, 1, 1, 16) == (1u << 5), "seat 5: port 2 bank 1 Data2");

  joypad_write_iobit(&s, 0x00);          /* both IOBits low -> second pairs */
  strobe(&s);
  check(shift_line(&s, 0, 0, 16) == (1u << 2), "seat 2: port 1 bank 2 Data1");
  strobe(&s);
  check(shift_line(&s, 0, 1, 16) == (1u << 3), "seat 3: port 1 bank 2 Data2");
  strobe(&s);
  check(shift_line(&s, 1, 0, 16) == (1u << 6), "seat 6: port 2 bank 2 Data1");
  strobe(&s);
  check(shift_line(&s, 1, 1, 16) == (1u << 7), "seat 7: port 2 bank 2 Data2");

  /* The ports select independently: bit 6 must not move port 2's bank. */
  joypad_write_iobit(&s, 0x80);          /* port 1 low, port 2 high */
  strobe(&s);
  check(shift_line(&s, 0, 0, 16) == (1u << 2), "port 1 on its second pair");
  strobe(&s);
  check(shift_line(&s, 1, 0, 16) == (1u << 4), "port 2 still on its first pair");
}

/* ── enabling a tap after boot leaves its seats plugged in ───────────── */

static void test_tap_enabled_after_reset(void)
{
  Snes s;
  int i;

  /* The order a real game uses: boot with no tap, then configure one. */
  memset(&s, 0, sizeof(s));
  joypad_set_multitap(0, 0);
  joypad_set_multitap(1, 0);
  joypad_reset_state();
  joypad_set_multitap(1, 1);
  joypad_write_iobit(&s, 0xc0);

  for (i = 0; i < 5; i++)
    check(joypad_get_connected(i) == 1,
          "seats are occupied by default after enabling a tap");

  strobe(&s);
  for (i = 0; i < 16; i++)
    joypad_read_port(&s, 1);
  check((joypad_read_port(&s, 1) & 3u) == 3u,
        "both first-pair seats report connected on the 17th bit");
}

/* ── tap on port 1 only: the lone pad moves to seat 4 ────────────────── */

static void test_tap_on_port_one(void)
{
  Snes s;

  reset_all(&s, 1, 0);
  joypad_set_pad(0, BTN_B);
  joypad_set_pad(4, BTN_R);
  joypad_write_iobit(&s, 0xc0);
  strobe(&s);
  check(shift_line(&s, 0, 0, 16) == BTN_B, "port 1 tap owns seat 0");
  strobe(&s);
  check(shift_line(&s, 1, 0, 16) == BTN_R,
        "the lone port 2 controller is seat 4");
}

int main(void)
{
  test_no_tap_unchanged();
  test_detection();
  test_bank_select();
  test_connection_bit();
  test_bank_counters_independent();
  test_auto_read_mapping();
  test_five_player_sequence();
  test_eight_players();
  test_tap_enabled_after_reset();
  test_tap_on_port_one();

  if (g_failures) {
    fprintf(stderr, "multitap_test: %d FAILURES\n", g_failures);
    return 1;
  }
  puts("multitap_test: PASS");
  return 0;
}
