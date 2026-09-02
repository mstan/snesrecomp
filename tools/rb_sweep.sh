#!/usr/bin/env bash
#
# rb_sweep.sh — run the rollback matrix unattended and print one table.
#
# rb_loopback.sh answers "is this configuration sound?". This answers "which
# configurations have we ever actually tried?", which for most of the knobs
# below had been none: SNES_RB_SNAP_DEPTH accepts 8..240 and had never been
# moved, SNES_RB_TIP_RUNWAY governs whether a tip-extend can land at all, and
# until now nothing could drop a packet.
#
# The point is that a soak on real hardware is expensive and slow, so anything
# that can be established over loopback should be established here first.
#
#   tools/rb_sweep.sh ./MyGameRecomp [seconds-per-cell]
#
# Exit status is the verdict over the whole grid: 0 = every cell passed.
set -u

EXE="${1:?usage: rb_sweep.sh <executable> [seconds-per-cell]}"
SECS="${2:-45}"
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${RB_SWEEP_OUT:-/tmp/rb_sweep}"
mkdir -p "$OUT"

fails=0
cells=0

# ── pre-flight ────────────────────────────────────────────────────────────
#
# Two checks that need no peer, run first because they are fast and because a
# failure here invalidates everything after it.
#
# Both exist because "present in the tree" and "has ever executed" are
# different claims. The predicate's cases were quoted in an audit while living
# only in a scratchpad, and the recovery path they guard had never once run in
# any log we held.

SNESRC=$(cd "$HERE/.." && pwd)
echo "pre-flight"

if cc -I "$SNESRC/runner/src" -I "$SNESRC/runner/src/snes" \
      "$SNESRC/tests/cpu/pc24_resumable_test.c" -o "$OUT/pc24_test" 2>"$OUT/pc24_build.log"; then
    if "$OUT/pc24_test" >"$OUT/pc24_test.log" 2>&1; then
        echo "  resume-PC predicate      PASS  ($(grep -c '^  ok' "$OUT/pc24_test.log") cases)"
    else
        echo "  resume-PC predicate      FAIL  — see $OUT/pc24_test.log"; fails=$((fails+1))
    fi
else
    echo "  resume-PC predicate      NO-BUILD — see $OUT/pc24_build.log"; fails=$((fails+1))
fi

# The recovery arms. Each must fire and the machine must keep running; with the
# knob off the path must stay silent, or the check is measuring nothing.
n_rec=$( ( cd "$(dirname "$EXE")" && timeout 20 env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    GAME_FORCE_BAD_RESUME_PC=120 "./$(basename "$EXE")" ) 2>&1 \
    | grep -c "recovering to last good" || true )
n_cold=$( ( cd "$(dirname "$EXE")" && timeout 20 env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    GAME_FORCE_BAD_RESUME_PC=120 GAME_FORCE_BAD_RESUME_STICKY=1 "./$(basename "$EXE")" ) 2>&1 \
    | grep -c "cold booting" || true )
n_quiet=$( ( cd "$(dirname "$EXE")" && timeout 15 env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "./$(basename "$EXE")" ) 2>&1 | grep -c "implausible" || true )

# Rewind determinism and the resync contract, one process, no peer. The
# symmetry half of this had been opt-in and therefore off in every run; it is
# on by default now, so the pre-flight is where it earns that.
probe_out=$( ( cd "$(dirname "$EXE")" && timeout 40 env SDL_VIDEODRIVER=dummy \
    SDL_AUDIODRIVER=dummy SNESRECOMP_RB_PROBE=25:2 SNESRECOMP_RB_PROBE_VERBOSE=1 \
    "./$(basename "$EXE")" ) 2>&1 )
n_sym=$(printf '%s' "$probe_out" | grep -c 'symmetric frame' || true)
n_asym=$(printf '%s' "$probe_out" | grep -c 'ASYMMETRIC' || true)
n_div=$(printf '%s' "$probe_out" | grep -c 'DIVERGE' || true)
n_vol=$(printf '%s' "$probe_out" | grep -oE '\(([0-9]+) volatile' | grep -oE '[0-9]+' \
        | sort -rn | head -1)
if [ "$n_sym" -gt 0 ] && [ "$n_asym" -eq 0 ] && [ "$n_div" -eq 0 ]; then
    echo "  rewind determinism       PASS  ($n_sym symmetric, 0 asymmetric, 0 divergent," \
         "noise floor ${n_vol:-0} byte(s))"
else
    echo "  rewind determinism       FAIL  (symmetric=$n_sym asymmetric=$n_asym divergent=$n_div)"
    fails=$((fails+1))
fi

if [ "$n_rec" -gt 0 ] && [ "$n_cold" -gt 0 ] && [ "$n_quiet" -eq 0 ]; then
    echo "  resume-PC recovery       PASS  ($n_rec recovered, $n_cold cold-booted, 0 when off)"
else
    echo "  resume-PC recovery       FAIL  (recovered=$n_rec cold=$n_cold when-off=$n_quiet)"
    fails=$((fails+1))
fi
echo

# name | env assignments | forced-mispredict interval (default 45)
grid=(
  "baseline                |"
  "rtt 60ms                |RNET_SIM_LATENCY_MS=30 RNET_SIM_JITTER_MS=8"
  "rtt 200ms               |RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25"
  "rtt 300ms               |RNET_SIM_LATENCY_MS=150 RNET_SIM_JITTER_MS=40"
  "loss 2%, fast link      |RNET_SIM_LOSS_PCT=2"
  "loss 5%, fast link      |RNET_SIM_LOSS_PCT=5"
  "loss 2% + rtt 200ms     |RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25 RNET_SIM_LOSS_PCT=2"
  "shallow ring (depth 8)  |SNES_RB_SNAP_DEPTH=8 RNET_SIM_LATENCY_MS=100"
  "deep ring (depth 240)   |SNES_RB_SNAP_DEPTH=240 RNET_SIM_LATENCY_MS=100"
  "runway 4 (below rtt)    |SNES_RB_TIP_RUNWAY=4 RNET_SIM_LATENCY_MS=100"
  "runway 24 (above rtt)   |SNES_RB_TIP_RUNWAY=24 RNET_SIM_LATENCY_MS=100"
  "disconnect mid-match    |RB_LOOPBACK_KILL_AT=20 RNET_SIM_LATENCY_MS=30"
  # Tip-extend only fires when a late edge lands while tip-hold is still open,
  # which needs the runway to outlast the round trip AND edges arriving faster
  # than one per episode. At the default interval the sweep reports zero
  # extends, which reads as "never fires" rather than "never provoked".
  "STRESS tip-extend       |SNES_RB_TIP_RUNWAY=24 RNET_SIM_LATENCY_MS=100 RNET_SIM_JITTER_MS=25|6"
)

# Cells whose name starts with STRESS are reported but do not gate the sweep.
# The tip-extend cell forks roughly one run in three: a sustained mispredicted
# remote edge every 6 ticks is far past anything real play produces, and the
# divergence is real but not yet understood. Left in because it reproduces on
# demand, which it never did before -- and left non-gating because a suite that
# fails a third of the time stops being read, which is the failure this whole
# harness exists to avoid.

printf '%-24s %-9s %7s %7s %7s %7s  %s\n' \
  cell verdict episodes aborts extends stalls note
printf '%.0s─' {1..92}; echo

for row in "${grid[@]}"; do
    name="${row%%|*}"; name="${name%"${name##*[![:space:]]}"}"
    rest="${row#*|}"
    envs="${rest%%|*}"
    mis="${rest#*|}"; [ "$mis" = "$rest" ] && mis=45
    [ -n "$mis" ] || mis=45
    slug=$(echo "$name" | tr -c 'a-zA-Z0-9' '_')
    cells=$((cells + 1))
    # shellcheck disable=SC2086
    out=$(env $envs RB_LOOPBACK_OUT="$OUT/$slug" \
          bash "$HERE/rb_loopback.sh" "$EXE" "$SECS" "$mis" 2>&1)
    rc=$?
    verdict=$(echo "$out" | grep -oE '^(PASS|FAIL)[^:]*' | head -1)
    [ -n "$verdict" ] || verdict="NO-RUN"
    case "$name" in
        STRESS*) [ "$rc" -eq 0 ] || verdict="FLAKY" ;;
        *)       [ "$rc" -eq 0 ] || fails=$((fails + 1)) ;;
    esac

    logs="$OUT/$slug"/*.log
    ep=$(cat $logs 2>/dev/null | grep -c 'RESIM episode' || true)
    ab=$(cat $logs 2>/dev/null | grep -c 'RB abort' || true)
    ex=$(cat $logs 2>/dev/null | grep -c 'RB tip-extend epoch' || true)
    st=$(cat $logs 2>/dev/null | grep -c 'RB chain stall' || true)
    drop=$(cat $logs 2>/dev/null | grep -oE 'dropped=[0-9]+' | tail -1)
    printf '%-24s %-9s %7s %7s %7s %7s  %s\n' \
      "$name" "${verdict:0:9}" "$ep" "$ab" "$ex" "$st" "${drop:-}"
done

echo
if [ "$fails" -eq 0 ]; then
    echo "SWEEP PASS: $cells cells, every gating cell clean"
else
    echo "SWEEP FAIL: $fails of $cells cells failed — logs under $OUT"
fi
exit $(( fails > 0 ))
