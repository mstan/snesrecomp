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
# Env passthrough: set SNES_RB_FORCE_FORK=N to exercise the fork cap and its
# recovery, which real forks no longer reach.
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

common=(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
        SNES_NETPLAY=1 SNES_NET_SLOTS=2 SNES_NET_MODE=rollback
        SNES_NET_DELAY="${SNES_NET_DELAY:-8}"
        SNES_RB_PREDICTION="${SNES_RB_PREDICTION:-12}"
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

ep_i=$(count initiator 'RESIM episode'); ep_f=$(count follower 'RESIM episode')
fk_i=$(count initiator 'FORK');          fk_f=$(count follower 'FORK')
echo
# An episode the peer never opened is normal (a NACK when it cannot reach the
# load tick); a large gap is not, so compare rather than demand equality.
gap=$(( ep_i > ep_f ? ep_i - ep_f : ep_f - ep_i ))
if [ "$((fk_i + fk_f))" -ne 0 ]; then
    echo "FAIL: $((fk_i + fk_f)) fork(s) — the peers disagreed on state"; rc=1
elif [ "$gap" -gt $(( (ep_i + 9) / 10 + 2 )) ]; then
    echo "FAIL: episode counts diverge ($ep_i vs $ep_f)"; rc=1
elif [ "$rc" -ne 0 ]; then
    echo "FAIL: a peer opened no episodes — nothing was exercised"
else
    echo "PASS: $ep_i episodes, no forks, peers agree"
fi
echo "logs: $OUT/initiator.log  $OUT/follower.log"
exit $rc
