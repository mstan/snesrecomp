# Rollback netcode (SNES)

Status: **landed, unsoaked** · opt-in via `SNES_NET_MODE=rollback` ·
depends on `lib/recomp-net` and `lib/retcomm-rbengine`

Delay-sync stays the default and is untouched. Rollback is a second admit
path behind the same `snes_netplay_*` facade, so a game that already drives
delay-sync needs **no source change** to run it — only the CMake opt-in and
the environment variable.

This is the SNES counterpart to psxrecomp's `docs/ROLLBACK_MOTK_HOOKUP.md`.
That document is a 115-section soak log of a shipped implementation; this one
is a design record for a fresh one. Read that file for the failure modes that
are platform-independent, and this one for what SNES changes.

---

## 1. Layers

```text
game main loop                     unchanged: poll_admit → RtlRunFrame → finish_frame
  └── snes_netplay                 facade + mode gate
        ├── snes_netplay_rb        this port: snapshots, digests, resim, episode wire
        ├── retcomm-rbengine       invent policy, input history, hash_confirm, snap ring
        └── recomp-net             RNetSession tips + RNetRbSession episode FSM
```

`retcomm-rbengine` is MotK-proven host policy with the PSX types removed, so
the admission scheduler, the hold-last invent, the hash-confirm watermark, and
the snapshot ring are shared with psxrecomp rather than rewritten here. What
this repo owns is everything genuinely SNES: what a snapshot contains, what a
digest covers, and how a resim runs.

## 2. Build and run

```sh
git submodule update --init --recursive lib/recomp-net lib/retcomm-rbengine
```

```cmake
snesrecomp_enable_recomp_net(MyGame)   # as before
snesrecomp_enable_rollback(MyGame)     # adds retcomm-rbengine + the rollback host
```

or set `-DSNESRECOMP_NET_ROLLBACK=ON` before including `runner.cmake`.

At runtime:

| Variable | Effect |
|----------|--------|
| `SNES_NET_MODE=rollback` | Use the rollback admit path (default: `delay`) |
| `SNES_RB_PREDICTION` | Prediction cap P in ticks (default 8) |
| `SNES_RB_SNAP_INTERVAL` | Snapshot every N ticks (default 1 — see §4) |
| `SNES_RB_SNAP_DEPTH` | Snapshot ring depth (default 40) |
| `SNES_RB_TIP_RUNWAY` | TipHold quiet window (default 12) |

`retcomm-rbengine`'s own `RBE_RB_*` scheduler knobs apply unchanged; see its
README.

A build without `SNESRECOMP_NET_ROLLBACK` ignores `SNES_NET_MODE` entirely,
and a rollback host that fails to start logs and falls back to delay-sync for
that session rather than leaving the game with no admit path.

## 3. What SNES makes easy

The PSX host carries a large apparatus this port simply does not need:

- **No mid-guest poll.** `RtlRunFrame` is a call that returns, so every
  snapshot and every rewind happens at a clean frame boundary. There is no
  basic-block-edge savestate poll, no `longjmp` resume, no top-level-resume
  recovery, and no null-PC repair — MotK §§ on all of that are inapplicable.
- **No media path.** No FMV, MDEC, CD, or BIOS. Every media/lockstep gate in
  `RbeSchedGates` is left NULL, which means invent is never held for media and
  auto-D always samples. MotK §§26, 50, 93–98, 109–115 have no analogue.
- **Digital pads only.** The stick-replace contract still runs (it is where
  the "predicted rows never get a bare deadband promote" invariant lives), but
  the analog machinery around it is inert.
- **Up to 8 seats.** With a Super Multitap configured the session carries more
  than two players; the driver loops over `slot_count` and the facade routes
  seats 2..7 through `RtlSetPadState`, so nothing in the resim path counts
  players. See [MULTITAP.md](MULTITAP.md).
- **Cheap snapshots.** A full SNES state is ~300 KB against the PSX's
  multi-megabyte VRAM + RAM, which is what makes §4 possible.

## 4. Snapshots are dense, on purpose

`SNES_RB_SNAP_INTERVAL` defaults to **1**: a snapshot before every tick.

MotK snapshots sparsely because it has to, and a large share of its soak log
is the consequence — episodes refused for want of a snapshot at the load tick,
`choose_load` clamps, dense-tip special cases, stale-snap diagnostics. At
~300 KB and ~60 Hz that is ~18 MB/s of memcpy and ~12 MB of ring for the
default depth of 40, which a SNES port can simply afford. Buying the whole
class of "no snapshot at the tick I need" out of existence is worth more than
the memory.

The ring is re-keyed onto the replayed timeline during a resim, and everything
past the target is dropped, so a later episode can never load a snapshot that
belongs to a discarded run.

**A snapshot keyed T is the state *before* tick T runs.** That is what
recomp-net's replay contract wants (`load_state(load)` then `advance_sim(t)`
for `t = load..target` re-runs the load tick itself). Keying it "after T"
instead replays every episode one tick short of its own mismatch.

## 5. The two things SNES gets wrong by default

Both were found by reading the emulator rather than by soaking, and both would
have presented as an unexplained mid-match desync.

### 5.1 The audio output ring is host state, and it is inside the savestate

`Dsp.sampleBuffer` / `sampleWrite` / `sampleRead` sit inside the frozen
`dsp_saveload` blob. But the **read cursor is advanced by the SDL audio
thread** at the host device's rate. It is wall-clock state by definition and
can never agree between two peers.

Consequences, both handled:

- **Digests exclude it.** `snes_state_digest.c` folds only the DSP prefix that
  precedes the ring. Had it folded the whole blob, the master digest would
  differ on *every* tick, every FRAME_COMMIT would mismatch, and the session
  would collapse into a permanent episode storm that looks nothing like an
  audio bug. `tests/netplay/rb_state_digest_test.c` pins this both ways — the
  ring excluded, and the field immediately before it still included.
- **Snapshot loads preserve it.** `RtlRollbackLoadFromMemory` lifts the ring
  out around the guest blob. Rewinding `sampleRead` would replay audio the
  player has already heard; rewinding `sampleWrite` below a cursor the audio
  thread has already passed makes `dsp_available()` — an unsigned difference —
  report about four billion.

Rewinding the producer cursor *alone* is, separately, how a resim discards the
audio it re-produced: `RtlAudioRewindProducer` puts it back where it stood
before the rewind. The renderer is off for the same span. Neither is optional;
this is `recomp-ai-rules/NETPLAY.md` §1 — the presented image and the audio
device are never simulation.

### 5.2 The savestate deliberately omits state rollback cannot omit

`apu_saveload` stops at `pad` and leaves the CPU→APU port-write scheduler out,
with a comment saying the queue is host lead better reset than restored. For a
one-off savestate load that is correct. For rollback it is not.

The queue schedules guest-visible port writes against `portClock`, a monotonic
APU-cycle count that a snapshot load does not rewind. After a rewind the
mapping from guest cycles to APU cycles no longer matches the SPC/DSP state
the snapshot just restored. Two peers rewinding at different ticks and
different depths then drift apart in APU handshake timing — a desync whose
cause is invisible at the point it shows up.

The same argument applies to `g_cpu.master_cycles` and the APU catch-up
accumulators. `RtlLoadSnapshot` re-anchors `beamMasterLast` to the *current*
`master_cycles` precisely because that clock does not rewind.

So a rollback snapshot is the guest blob **plus a sim-residue chunk**
(`RtlRollbackSaveToMemory`): `g_cpu`, the APU pacing accumulators and frame
anchors, and `ApuPortSched`. These blobs are in-process and in-memory only —
never a file, never a wire payload — and carry no cross-build compatibility
promise.

### 5.3 Why delay-sync never noticed

Delay-sync compares **input** hashes (`rnet_session_input_desync`), never
state. Both defects above are invisible to it: they perturb APU timing and
audio-ring bookkeeping, not the pad stream. Rollback is the first thing in
this repo that compares guest state between peers, so it is the first thing
that can see them.

## 6. Digest partitions

The digest walks the same `*_saveload` serializers the snapshot does, so the
digest domain and the snapshot domain cannot drift apart as the emulator grows
fields — state digested but not restored forks every rewind, state restored
but not digested lets a fork go unseen. The audio ring in §5.1 is the single
deliberate subtraction.

| Partition | Covers |
|-----------|--------|
| `cpu` | interpreter `Cpu` register file |
| `wram` | 128 KiB WRAM + `Snes` tail regs + joypad/multitap shift state |
| `apu` | SPC700 + APU RAM + DSP **minus the output ring** |
| `ppu` | PPU regs + VRAM / CGRAM / OAM |
| `dma` | DMA/HDMA channel state |
| `cart` | SRAM + SA-1 / Cx4 / DSP-1 |

`master` folds the six in a fixed order and is what rides the FRAME_COMMIT and
POST wires. The partitions exist so a fork names the subsystem that moved:
"the state differs" is not a diagnosis, "the APU differs and everything else
matches" is (`recomp-ai-rules/NETPLAY.md` §5). A baseline fork logs the
partition; `snes_netplay_rb_last_fork()` exposes the latest one.

## 7. Episode path

Standard recomp-net handshake, driven from `poll_admit`:

```text
Live ─mispredict─> begin_episode ─> seal_inputs ─> RB_SYNC BEGIN + RB_SEAL_ROWS
     ─peer rows complete─> load_state ─> RB_BASELINE ─> replay(load..target)
     ─> RB_POST ─> match ─> TipHold ─> Live
                └─ diverge ─> abort + cooldown
```

- Epoch ids are partitioned by initiator slot (`counter << 1 | slot`) so
  concurrent dual initiation cannot collide; the lower slot wins the tie-break
  and the loser yields and follows.
- A follower with no snapshot at the requested load tick sends `OP_NACK`
  carrying its confirmed frontier, and the initiator demotes to a mutually
  provable tick instead of guessing `load - 1`.
- Replay runs **inline**. On SNES a resim tick is a plain `RtlRunFrame` and
  the span is bounded by the tip runway, so the whole replay is tens of frames
  of emulation with no host stack to unwind.
- A baseline digest fork means the two peers do not agree on the state they
  are about to replay *from*, so the replay is doomed before it starts: the
  episode aborts and names the forked partition rather than replaying into a
  guaranteed POST mismatch.
- Any failure aborts loudly with a cooldown. Nothing here continues silently
  after a fork — `recomp-ai-rules/NETPLAY.md` §5, a digest mismatch is a stop.

## 8. SNES pad polarity

`retcomm-rbengine` fills an unknown invent row with `0xFFFF`, because PSX pads
are active low. **SNES pads in this runner are 12-bit active high**, so that
value means every button held. Two guards, in `snes_netplay_rb.c`:

- a neutral row is seeded per seat at start, so hold-last almost never reaches
  the fallback;
- `rb_row_sanitize` maps the `0xFFFF` sentinel to `0` and masks to 12 bits on
  every row entering history, a seal, or the sim. A genuine SNES row can never
  be `0xFFFF`, so the sentinel is unambiguous.

`rbe_ih_invent_idle` is never called: its neutral is the same PSX-shaped
value, and there is no seal gap-fill path here that needs it.

## 9. Not ported from MotK

Named so nobody assumes they are present:

- Ownership chains / chain-replay, and the whole `Replay ownership` protocol
  (MotK §§47–53, 60–63, 70–73, 85–87).
- Tip-extend re-replay scheduling beyond the library's own `extend_target`
  (§§66–69, 78–82).
- Light-tip depth heuristics beyond setting `light_tip_max_depth` to match the
  tip runway.
- Every FMV / media-keyframe path (§§26, 50, 93–98, 109–115) — inapplicable.
- Netplay save/load under an active episode (§§94–95). Savestate transfer
  still runs on the delay-sync path only.
- Cross-OS pacing diagnostics beyond what `retcomm-rbengine` provides.

## 10. What has and has not been verified

Verified here:

- `retcomm-rbengine` and `recomp-net` build and their own suites pass against
  the pins this repo vendors.
- Every touched translation unit compiles clean at `-Wall -Wextra`.
- `snesrecomp_enable_rollback` configures, builds `retcomm_rbengine`, and puts
  it on a game target's link line.
- `tests/netplay/rb_state_digest_test.c` passes, and was mutation-checked: it
  fails when the ring exclusion is removed, and fails again when the exclusion
  is widened by one field.

**Not verified — this needs a two-machine soak.** No live match has been run.
Per `recomp-ai-rules/PRINCIPLES.md` and the workspace standing rulings, the
code under test and its oracle run as **two processes**, and the gameplay
verdict belongs to a human at a real controller. Until that soak happens,
treat §7 as a design that compiles, not as a shipped path.

The first soak should watch, in order:

1. **FRAME_COMMIT agreement with zero episodes.** Force `P=0` so no invent
   happens and both peers run pure lockstep. If master digests diverge here,
   nothing downstream matters — and the partition name says where to look.
2. **First invent, first episode.** One held button, one dropped packet.
   Confirm the episode opens, replays, and POSTs a match.
3. **Audio across a rewind.** The ring work in §5.1 is what this exercises;
   a click or a repeat means the producer rewind is wrong.
4. **Sustained play.** Episode rate, resim ticks/sec, and desync count are on
   `snes_netplay_rb_*` accessors for the diagnostics dump.
