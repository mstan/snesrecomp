# Super Multitap (up to 8 players)

Status: **landed, unsoaked** · off by default · `SNES_MULTITAP=port1|port2|both`

snesrecomp emulates the Super Multitap (Hudson HUD-101), which raises a title
from two seats to five with one tap, or eight with a tap in each port. It is
off unless a game or a launch flag asks for it, and with it off every path is
byte-identical to the two-pad machine that shipped before — see §5.

This mirrors psxrecomp's SCPH-1070 support (`runtime/include/sio.h`), and
deliberately uses the same seat vocabulary so the two engines describe the
same player in the same words.

---

## 1. Hardware model

Three facts drive everything else. Both come from the
[SNESdev wiki "Multitap" page](https://snes.nesdev.org/wiki/Multitap) and the
[Super Famicom development wiki "Controllers" page](https://wiki.superfamicom.org/controllers),
not from reading another emulator.

**Two data lines per port.** Reads of `$4016` (port 1) and `$4017` (port 2)
return Data1 in bit 0 and Data2 in bit 1. A standard controller drives Data1
only; a multitap drives both, reporting two pads at once. Before this change
the runner returned Data1 alone and the automatic-read registers for Data2
(`$421C`-`$421F`) were hardcoded to zero.

**`$4201` selects the pair.** `$4201` is an open-collector output whose bit 6
drives port 1's IOBit pin and bit 7 drives port 2's. A standard controller
ignores IOBit. A tap uses its port's line to choose which pair of its four
pads it reports: **IOBit 1 selects the first pair, IOBit 0 the second.** Bit 7
is also the PPU counter-latch line, which is why `$4201` already had a meaning
in this runner before multitap existed — that behaviour is untouched.

**17 bits per pad.** A tap reports the usual 16 button bits and then one more,
which is **1 when that tap slot has a controller in it and 0 when it is
empty**. Reads past that return 1s, as a standard controller does after bit 15.

Detection, per the wiki: with the `$4016` strobe **high** a tap reports 1 on
Data2, so eight reads give `$FF`; with the strobe **low** it reports pad data,
which is `$FF` only if all eight of those buttons are held. A port with no tap
reports 0 on Data2 and so can never be mistaken for one.

## 2. Seat numbering

Following psxrecomp's multitap exactly:

| Configuration | Seats |
|---|---|
| no tap | port 1 = seat 0, port 2 = seat 1 |
| tap on port 1 | tap = seats 0-3, port 2 pad = seat 4 |
| tap on port 2 | port 1 pad = seat 0, tap = seats 1-4 |
| taps on both | port 1 tap = seats 0-3, port 2 tap = seats 4-7 |

**Tap on port 2 is the five-player layout** every commercial multitap title
uses: the tap goes in port 2 and the first player keeps port 1.

Note what this means for a two-player game that gains a tap on port 2: seat 1
stops being the port-2 controller and becomes the tap's *first* pad. That is
the hardware's numbering, not a choice, and it is why `RtlRunFrame`'s player-2
half lands on seat 1 either way.

## 3. Using it from a game

`RtlRunFrame`'s packed word still owns seats 0 and 1 and is unchanged, so no
existing port has to touch its frame loop. Extra seats are additive:

```c
RtlSetMultitap(1, 1);                 /* tap in console port 2 -> 5 seats */
RtlSetPadConnected(4, 0);             /* fifth seat empty this match */

for (;;) {
    RtlSetPadState(2, ReadPad(2));    /* seats 2..7 */
    RtlSetPadState(3, ReadPad(3));
    RtlRunFrame(p0 | (p1 << 12) | GetActiveControllers());
}
```

| Call | Purpose |
|---|---|
| `RtlSetMultitap(port, on)` | `port` 0 = console port 1, 1 = console port 2 |
| `RtlGetMultitap(port)` | current setting |
| `RtlPlayerCount()` | seats the configuration reaches: 2, 5, or 8 |
| `RtlSetPadState(seat, buttons)` | 12-bit buttons for seats 2..7 (0/1 also work) |
| `RtlSetPadConnected(seat, on)` | drives the tap's 17th bit for that seat |

`SNES_MULTITAP=port1|port2|both|off` overrides whatever the game asked for, so
a launcher or a soak script can turn a tap on without a rebuild. An explicit
launch flag outranks a built-in default.

Opposing d-pad directions are filtered on every seat, not just the first two,
because guest code is entitled to assume a real d-pad.

## 4. Netplay

recomp-net already carries up to `RNET_MAX_SLOTS` (8) seats, so the wire
needed nothing new. `SnesNetplayConfig.slot_count` (env `SNES_NET_SLOTS`)
opens a wider session, and both the delay-sync and rollback admit paths
publish every seat.

Seats 2..7 do not fit `snes_netplay_published_inputs()`'s packed word, so
publish pushes them into the runtime through `RtlSetPadState` — the same door
a local multitap game uses. **A game therefore needs no netplay-specific code
to gain seats beyond the second.**

Starting a session with more seats than the port configuration can route is
**refused, not degraded**: a peer quietly running two seats while another runs
five would desync on input and report nothing that names the cause
(`recomp-ai-rules/NETPLAY.md` §4). Every peer must configure the same taps.

Rollback is unaffected in shape — `snes_netplay_rb` loops over
`slot_count` — but note that the multitap's shift state is now part of the
digest; see §6.

## 5. What stays byte-identical with no tap

This matters more than the feature: roughly two dozen shipping ports are on
this path and none of them asked for a multitap.

- `$4016`/`$4017` return Data1 in bit 0 exactly as before; Data2 reads 0 with
  no tap, so the returned byte is unchanged.
- `$421C`-`$421F` still read 0.
- The automatic-read registers still answer from **live** pad state rather
  than a latched copy, so a game that reads `$4218` without ever enabling the
  automatic-read handshake keeps getting data instead of zeroes. Only a tap
  configuration switches to latched words, which is what the five-player
  sequence in §7 requires.
- While the strobe is high a port with no tap still reports its live first
  button, which `tests/joypad/manual_joypad_test.c` pins.

## 6. Savestates and rollback digests

Savestate format goes to **v8**, adding a multitap chunk: seats, IOBit lines,
per-bank shift counters and the latched automatic-read words. Older states
still load (`RTL_SAV_VERSION_MIN` is unchanged at 4) and come up with no tap
configured, which is the pre-multitap machine exactly.

The shift state has to be in there because it is **guest-visible mid-frame**:
a state saved between the two halves of a five-player read must resume in the
same bank at the same bit. For the same reason it is folded into the rollback
`wram` digest partition — a peer that rewound into the middle of a read has to
come back to the same place, and the digest domain must equal the snapshot
domain (see `docs/ROLLBACK.md` §6).

## 7. The five-player sequence

The wiki's reading procedure, which `tests/joypad/multitap_test.c` walks in
order:

1. `$4201.7 = 1` — select the tap's first pair.
2. Let the automatic read capture seats 0, 1, 2 into JOY1/JOY2/JOY4.
3. Poll `HVBJOY.0` until the automatic read finishes.
4. Read `$4017` once more for the 17th bit of seats 1 and 2.
5. `$4201.7 = 0` — select the second pair.
6. Read 16 bits for seats 3 and 4.
7. Read once more for their 17th bits.
8. `$4201.7 = 1` again for the next frame.

Step 4 is the load-bearing one, and it is why the automatic read here does a
real latch and sixteen clocks rather than just filling registers: those clocks
leave the selected bank's shift counter sitting on the 17th bit, which is
where step 4 expects to find it. An implementation that only filled
`$4218`-`$421F` would leave the counter at 0 and hand step 4 a button bit
instead of a connection flag.

The automatic-read register map, with a tap in port 2:

| Register | Line | Seat (tap on port 2) |
|---|---|---|
| `$4218/9` (JOY1) | port 1 Data1 | 0 |
| `$421A/B` (JOY2) | port 2 Data1 | 1 |
| `$421C/D` (JOY3) | port 1 Data2 | — (nothing on port 1) |
| `$421E/F` (JOY4) | port 2 Data2 | 2 |

Only three seats are reachable by automatic read; seats 3 and 4 need the
manual sequence above. That is a hardware limit, not an implementation one.

Note the wiki's warning that some third-party taps transition slowly from
`$4201.7 = 0` back to `1`, which is why it recommends reading the first pair
first. Nothing here models that delay — a title that depends on it would need
a timing model this implementation does not have.

## 8. What has and has not been verified

Verified here:

- `tests/joypad/multitap_test.c` covers detection, bank select via both IOBit
  lines, the 17th connection bit, independent per-bank shift counters, the
  automatic-read seat map, the full five-player sequence, the eight-player
  dual-tap layout, and the port-1-tap seat shuffle. It was mutation-checked:
  it fails when the IOBit sense is inverted, when the 17th bit always reports
  connected, when the automatic read leaves the shift counter at 0, when Data2
  is sampled one bit ahead of Data1, and when the latch stops resetting the
  counters.
- The pre-existing `manual_joypad_test` and `auto_joypad_test` still pass
  unchanged, which is the no-tap compatibility claim in §5.
- Every touched translation unit compiles clean at `-Wall -Wextra`.

**Not verified — this needs a real ROM.** No multitap title has been run. The
protocol above is implemented from documentation; the natural first check is a
differential run against a libretro core in `tools/snesref` (§"Deterministic
capture" in its README) with a five-player game, comparing the WRAM trace
frame by frame through the title's controller-detection routine. Until that
happens, treat this as a documented implementation rather than a verified one.

Two things a soak should watch first:

1. **Detection.** If a title decides there is no tap, the fault is in §1's
   strobe-high behaviour or in the Data2 line, and it will show up in the
   first few frames of the controller-setup routine.
2. **Seats 3 and 4 specifically.** They are the only ones that depend on the
   manual bank switch and the shift-counter position from step 4. Seats 0-2
   working while 3-4 read garbage points straight at the automatic read's
   counter handling.
