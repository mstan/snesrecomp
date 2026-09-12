#!/usr/bin/env bash
#
# rb_loopback.sh — two-process rollback soak for any snesrecomp port.
#
# Runs the same executable as both peers over UDP loopback and reports what
# the rollback machinery actually did. Written because every rollback defect
# found in this engine so far — the seal-row wire encoding, an episode target
# past the local tip, the peer POST gated on local stage, a fork cap with no
# way to lift — was caught by a pair like this and NOT by any single-process
# test. It lived in a scratchpad for four of those; it belongs in the tree.
#
#   tools/rb_loopback.sh ./MyGameRecomp [seconds] [force-mispredict-interval]
#
# Env passthrough:
#   SNES_RB_FORCE_FORK=N       exercise the fork cap and its recovery, which
#                              real forks no longer reach.
#   RNET_SIM_LATENCY_MS=N      add N ms one-way to EACH peer's receive path
#                              (so ~2N ms of added RTT). Without this the run
#                              is a zero-latency link and cannot reach the
#                              paths that only exist under real RTT.
#   RNET_SIM_JITTER_MS=N       uniform +/-N ms around that, reordering allowed.
#   RNET_SIM_LOSS_PCT=N        drop N% of arriving datagrams on EACH side.
#                              Latency and jitter delay and reorder; only loss
#                              reaches the paths that must survive a message
#                              never arriving. Independent of latency.
#   RNET_SIM_SEED=N            make a jittered/lossy run repeat.
#   SNES_RB_SNAP_DEPTH=N       snapshot ring depth (8-240). Never swept before.
#   SNES_RB_TIP_RUNWAY=N       tip-hold quiet window in ticks.
#   RB_LOOPBACK_KILL_AT=N      kill the FOLLOWER N seconds in, to check the
#                              survivor degrades rather than hangs. The verdict
#                              flips to "did the initiator keep running and
#                              exit cleanly", since episode accounting cannot
#                              balance against a peer that is gone.
#
# Exit status is the verdict: 0 = both peers agreed, non-zero = something to
# look at. The logs are left behind either way.
set -u

EXE="${1:?usage: rb_loopback.sh <executable> [seconds] [mispredict-interval]}"
SECS="${2:-45}"
MISPREDICT="${3:-45}"
OUT="${RB_LOOPBACK_OUT:-/tmp/rb_loopback}"

[ -x "$EXE" ] || { echo "rb_loopback: $EXE is not executable" >&2; exit 2; }
EXE_DIR=$(cd "$(dirname "$EXE")" && pwd)
EXE_BIN=$(basename "$EXE")
mkdir -p "$OUT"

# Anchored, so this cannot match the harness or a shell that merely mentions
# the name — an unanchored pkill here once killed five unrelated runtimes.
pkill -f "^\./${EXE_BIN}\$" 2>/dev/null
sleep 1

# Loopback RTT is ~0, and several rollback paths only exist above it: with no
# added latency the peer's OP_COMMIT lands before TipHold can ever receive a
# late edge, so the tip-extend branch is unreachable and the soak reports PASS
# on a machine it never ran. RNET_SIM_LATENCY_MS is per-process and one-way,
# so the added round trip is DOUBLE this. Set explicitly for both peers rather
# than exported and left to leak — that is how the force-knob bug happened.
common=(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
        SNES_NETPLAY=1 SNES_NET_SLOTS=2 SNES_NET_MODE=rollback
        SNES_NET_DELAY="${SNES_NET_DELAY:-8}"
        SNES_RB_PREDICTION="${SNES_RB_PREDICTION:-12}"
        RNET_SIM_LATENCY_MS="${RNET_SIM_LATENCY_MS:-0}"
        RNET_SIM_JITTER_MS="${RNET_SIM_JITTER_MS:-0}"
        RNET_SIM_LOSS_PCT="${RNET_SIM_LOSS_PCT:-0}"
        RNET_SIM_SEED="${RNET_SIM_SEED:-0}"
        SNES_RB_SNAP_DEPTH="${SNES_RB_SNAP_DEPTH:-0}"
        SNES_RB_TIP_RUNWAY="${SNES_RB_TIP_RUNWAY:-0}"
        SNES_NET_TRANSPORT=udp)

# Every validation knob, pinned OFF for the follower as a GROUP rather than
# one at a time. `env` does not clear the caller's environment, so a knob
# exported for the initiator silently reaches the follower too and both peers
# inject. That has now happened twice: once making a lockstep test look like it
# failed on the peer being measured, and once making a forced boot fork agree
# with itself because both sides perturbed identically. Add new knobs HERE, not
# to the follower's line.
follower_off=(SNES_RB_FORCE_MISPREDICT=0
              SNES_RB_FORCE_FORK=0
              SNES_RB_FORCE_BOOT_FORK=0
              SNES_RB_FORCE_MOD_MISMATCH=0
              SNES_RB_FORCE_MODSET=)

cd "$EXE_DIR" || exit 2
env "${common[@]}" SNES_NET_SLOT=0 SNES_NET_INPUT_PLAYER=0 \
    SNES_NET_BIND=127.0.0.1:9700 SNES_NET_PEER=127.0.0.1:9701 \
    SNES_RB_FORCE_MISPREDICT="$MISPREDICT" \
    SNES_RB_FORCE_FORK="${SNES_RB_FORCE_FORK:-0}" \
    "./$EXE_BIN" >"$OUT/initiator.log" 2>&1 &
sleep 1
env "${common[@]}" SNES_NET_SLOT=1 SNES_NET_INPUT_PLAYER=1 \
    SNES_NET_BIND=127.0.0.1:9701 SNES_NET_PEER=127.0.0.1:9700 \
    "${follower_off[@]}" \
    "./$EXE_BIN" >"$OUT/follower.log" 2>&1 &

KILL_AT="${RB_LOOPBACK_KILL_AT:-0}"
if [ "$KILL_AT" -gt 0 ] 2>/dev/null; then
    # Disconnect mid-match. The survivor must notice, degrade and exit cleanly;
    # what it must NOT do is wedge waiting on a peer that will never answer,
    # which is the failure every stage watchdog exists to prevent.
    sleep "$KILL_AT"
    kill %2 2>/dev/null
    echo "rb_loopback: killed the follower at ${KILL_AT}s" >&2
    sleep $(( SECS > KILL_AT ? SECS - KILL_AT : 5 ))
else
    sleep "$SECS"
fi
pkill -f "^\./${EXE_BIN}\$" 2>/dev/null
sleep 1

# grep -c prints 0 AND exits 1 when there is no match, so `|| echo 0` would
# emit the count twice and every later arithmetic test would choke on it.
count() {
    [ -f "$OUT/$1.log" ] || { echo 0; return; }
    grep -c "$2" "$OUT/$1.log" 2>/dev/null || true
}
rc=0
printf '%-11s %8s %7s %6s %6s %6s %6s\n' role episodes aborts forks pcap late resim
for role in initiator follower; do
    ep=$(count "$role" 'RESIM episode')
    ab=$(count "$role" 'RB abort')
    fk=$(count "$role" 'FORK')
    pc=$(count "$role" 'pcap FREEZE enter')
    lt=$(count "$role" 'forced late row')
    printf '%-11s %8s %7s %6s %6s %6s %6s\n' "$role" "$ep" "$ab" "$fk" "$pc" "$lt" "$ep"
    [ "$ep" -gt 0 ] || rc=1          # no episodes means nothing was exercised
done

# Count by ROLE and in BOTH directions.
#
# Counting per process made a spurious follow episode (a tip-extend mistaken
# for a fresh BEGIN) cancel against a real refusal instead of showing up. And
# counting only one direction assumed only the initiator ever initiates, which
# is true only while its injector is the sole source of mispredicts -- add
# packet loss and the follower starts opening its own episodes from organic
# ones. That assumption cost a false FAIL on the first lossy cell that also
# had latency.
#
# Every episode one side opens is answered by the other exactly once: followed,
# refused, or (only when the link can drop) never heard at all.
ini_a=$(count initiator 'RESIM episode.*initiator')
ini_b=$(count follower  'RESIM episode.*initiator')
fol_a=$(count initiator 'RESIM episode.*follower')
fol_b=$(count follower  'RESIM episode.*follower')
ref_a=$(count initiator 'RB follow refused')
ref_b=$(count follower  'RB follow refused')
ep_i=$(( ini_a + ini_b ))
ep_f=$(( fol_a + fol_b ))
nack_i=$(( ref_a + ref_b ))
fk_i=$(count initiator 'FORK');          fk_f=$(count follower 'FORK')
ab_i=$(count initiator 'RB abort')
# NACK specifically, not all aborts: only a NACK stops the follower from
# opening the episode at all. An initiator abort that lands AFTER the follower
# opened (a POST timeout, say) leaves both counts equal and must not be
# expected to show up as a gap — that miscount produced a residual of -1.
# Refusals of a BEGIN, counted at the peer that made them and tagged alike
# whatever the reason. NOT 'peer NACK' on the initiator: that also counts
# declined tip-extends, which refuse an EXTENSION of an episode that already
# opened and so must not appear in this ledger.
# Timeouts, both sides. On a lossy link a BEGIN can simply never arrive:
# the follower cannot refuse an episode it never heard about, so such an
# episode is neither followed NOR refused, and the exact ledger below has a
# third category it cannot see. The initiator's watchdog is what notices, and
# that is correct behaviour rather than a defect -- so under configured loss
# the identity relaxes to a bound. With no loss it stays exact, which is what
# it has been across every run measured so far.
timeout_i=$(( $(count initiator 'timed out waiting') + $(count follower 'timed out waiting') ))
echo
# An episode the peer never opened is normal — it NACK'd before logging one,
# and the initiator recorded an abort for it. So the counts are not meant to
# be equal, they are meant to BALANCE:
#
#     episodes initiated by either side
#       - episodes followed by either side
#       - BEGINs either side refused                  == 0
#     (== at most the timeout count, when loss is configured)
#
# Measured exact across 14 runs spanning two builds, three link latencies and
# both old verdicts. The tolerance this replaces ("gap no more than 10% + 2")
# was tuned on a zero-latency link where the gap is always 0; at 60 ms RTT the
# gap lands right on the threshold and the verdict flapped run to run, which
# cost a real afternoon chasing a regression that was never there. A residual
# is a genuine unaccounted-for episode; a tolerance is a coin flip.
#
# Only the initiator injects here (the follower's knobs are pinned off), so
# the difference is one-directional by construction.
gap=$(( ep_i - ep_f ))
resid=$(( gap - nack_i ))
# Any deliberate pre-match refusal. Counted as a class rather than one string
# at a time: the boot-digest stop needed this, then the mod-set stop needed it
# again, and grading a safety feature as "a peer opened no episodes" is a
# mistake worth making impossible rather than twice.
# Counted on BOTH peers: a refusal is not always symmetric. The boot digest is
# detected by each side independently, but a mod-set mismatch is detected by
# whichever side RECEIVES the differing identity — so looking only at the
# initiator missed it entirely and reported the refusal as a failure.
refused_boot=$(( $(count initiator 'BOOT DIGEST MISMATCH\|MOD SETS DIFFER\|MOD SET NOT AGREED') \
               + $(count follower  'BOOT DIGEST MISMATCH\|MOD SETS DIFFER\|MOD SET NOT AGREED') ))
if [ "$refused_boot" -gt 0 ] && [ "$ep_i" -eq 0 ]; then
    why=$(grep -ohE 'BOOT DIGEST MISMATCH|MOD SETS DIFFER|MOD SET NOT AGREED' "$OUT"/*.log \
          2>/dev/null | head -1)
    echo "PASS (refused): ${why:-a pre-match check} stopped the match before it" \
         "started — 0 episodes, which is the point"
elif [ "$KILL_AT" -gt 0 ] 2>/dev/null; then
    # The ledger cannot balance against a peer that stopped answering, so the
    # question becomes survival: did the initiator keep simulating after the
    # follower vanished, and did it say why rather than freeze?
    ticks_after=$(count initiator 'peer_gone\|peer gone\|RB abort')
    if [ "$ep_i" -eq 0 ]; then
        echo "FAIL: initiator opened no episodes before the disconnect"; rc=1
    elif [ "$fk_i" -ne 0 ]; then
        echo "FAIL: initiator forked ($fk_i)"; rc=1
    else
        echo "PASS (disconnect): initiator ran $ep_i episodes, survived the" \
             "follower going away, $ticks_after abort/peer-gone line(s)"
    fi
elif [ "$((fk_i + fk_f))" -ne 0 ]; then
    echo "FAIL: $((fk_i + fk_f)) fork(s) — the peers disagreed on state"; rc=1
elif [ "$resid" -gt 0 ] && [ "$resid" -le "$timeout_i" ]; then
    # Explained, not exact. A BEGIN can simply be lost -- UDP drops on any
    # link, not only when RNET_SIM_LOSS_PCT is set, and the peer cannot refuse
    # an episode it never heard of. Gating this on CONFIGURED loss made a
    # zero-loss cell fail roughly one run in five on a dropped datagram, which
    # is the cry-wolf failure this suite exists to avoid. The invariant that
    # actually holds is "every unaccounted episode is explained by a timeout",
    # which is true whether the loss was asked for or not.
    echo "PASS (explained): $ep_i episodes, no forks, $resid unaccounted but" \
         "covered by $timeout_i timeout(s) — a lost BEGIN is never answered"
elif [ "$resid" -ne 0 ]; then
    echo "FAIL: $resid episode(s) unaccounted for (initiator $ep_i, follower" \
         "$ep_f, refused by follower $nack_i, initiator timeouts $timeout_i)"; rc=1
elif [ "$rc" -ne 0 ]; then
    echo "FAIL: a peer opened no episodes — nothing was exercised"
else
    echo "PASS: $ep_i episodes, no forks, every episode accounted for"
fi
echo "logs: $OUT/initiator.log  $OUT/follower.log"
exit $rc
