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
#   RNET_SIM_SEED=N            make a jittered run repeat.
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
        RNET_SIM_SEED="${RNET_SIM_SEED:-0}"
        SNES_NET_TRANSPORT=udp)

cd "$EXE_DIR" || exit 2
env "${common[@]}" SNES_NET_SLOT=0 SNES_NET_INPUT_PLAYER=0 \
    SNES_NET_BIND=127.0.0.1:9700 SNES_NET_PEER=127.0.0.1:9701 \
    SNES_RB_FORCE_MISPREDICT="$MISPREDICT" \
    SNES_RB_FORCE_FORK="${SNES_RB_FORCE_FORK:-0}" \
    "./$EXE_BIN" >"$OUT/initiator.log" 2>&1 &
sleep 1
# The validation knobs are pinned OFF here, not merely left unset: `env` does
# not clear the caller's environment, so a knob exported for the initiator
# would silently reach the follower too and both peers would inject. That
# happened, and it made a lockstep test look like it had failed on the peer
# being measured.
env "${common[@]}" SNES_NET_SLOT=1 SNES_NET_INPUT_PLAYER=1 \
    SNES_NET_BIND=127.0.0.1:9701 SNES_NET_PEER=127.0.0.1:9700 \
    SNES_RB_FORCE_MISPREDICT=0 SNES_RB_FORCE_FORK=0 \
    "./$EXE_BIN" >"$OUT/follower.log" 2>&1 &

sleep "$SECS"
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

# Count by ROLE, not by process: an episode line says which role opened it,
# and either process can initiate. Counting every line per process made a
# spurious follow episode (a tip-extend mistaken for a fresh BEGIN) cancel out
# against a real refusal instead of showing up.
ep_i=$(count initiator 'RESIM episode.*initiator')
ep_f=$(count follower 'RESIM episode.*follower')
fk_i=$(count initiator 'FORK');          fk_f=$(count follower 'FORK')
# NACK specifically, not all aborts: only a NACK stops the follower from
# opening the episode at all. An initiator abort that lands AFTER the follower
# opened (a POST timeout, say) leaves both counts equal and must not be
# expected to show up as a gap — that miscount produced a residual of -1.
# Refusals of a BEGIN, counted at the peer that made them and tagged alike
# whatever the reason. NOT 'peer NACK' on the initiator: that also counts
# declined tip-extends, which refuse an EXTENSION of an episode that already
# opened and so must not appear in this ledger.
nack_i=$(count follower 'RB follow refused')
echo
# An episode the peer never opened is normal — it NACK'd before logging one,
# and the initiator recorded an abort for it. So the counts are not meant to
# be equal, they are meant to BALANCE:
#
#     initiated(A) - followed(B) == BEGINs B refused
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
if [ "$((fk_i + fk_f))" -ne 0 ]; then
    echo "FAIL: $((fk_i + fk_f)) fork(s) — the peers disagreed on state"; rc=1
elif [ "$resid" -ne 0 ]; then
    echo "FAIL: $resid episode(s) unaccounted for (initiator $ep_i, follower" \
         "$ep_f, refused by follower $nack_i)"; rc=1
elif [ "$rc" -ne 0 ]; then
    echo "FAIL: a peer opened no episodes — nothing was exercised"
else
    echo "PASS: $ep_i episodes, no forks, every episode accounted for"
fi
echo "logs: $OUT/initiator.log  $OUT/follower.log"
exit $rc
