#!/usr/bin/env bash
# Gate runner for the SMW co-simulation A-side (SNES_COSIM.md "Validation gates").
# Run AFTER building build/smw_cosim.exe. ALL of these must pass before any
# A-vs-B (recomp-vs-interp816) result is trustworthy — the whole point is that
# the tool cannot be silently wrong.
#
#   ROM=/path/smw.sfc ./gates.sh gate1     # A-vs-A determinism: MUST be 0 divergence
#   ./gates.sh gate3                       # injected fault: MUST halt ~cp20, name 'ram'
#   ./gates.sh gate4                       # hash-vs-byte audit: no AUDIT-FAIL lines
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="$HERE/build/smw_cosim.exe"
ROM="${ROM:-$HERE/../../SuperMarioWorldRecomp/smw.sfc}"
COORD="python $HERE/../tools/snes_cosim.py"
A="$EXE $ROM"

[ -x "$EXE" ] || { echo "build $EXE first (cmake --build build)"; exit 1; }
[ -f "$ROM" ] || { echo "ROM not found: $ROM (set ROM=...)"; exit 1; }

case "${1:-help}" in
  gate1)  # two instances of the SAME build — must never diverge
    echo "== Gate 1: A-vs-A determinism (expect: no divergence) =="
    $COORD --a-cmd "$A" --b-cmd "$A" --stride 1 --max "${MAX:-500}" ;;
  gate3)  # flip one WRAM byte in B after cp20 — must halt ~cp21, only 'ram' splits
    echo "== Gate 3: injected fault (expect: halt ~cp21, sub 'ram' only) =="
    $COORD --a-cmd "$A" --b-cmd "$A" --stride 1 --max 200 \
           --inject "ram:1000:255" --inject-at 20 ;;
  gate4)  # periodic full byte re-hash vs the chained hash
    echo "== Gate 4: hash-vs-byte audit (expect: no AUDIT-FAIL) =="
    $COORD --a-cmd "$A" --b-cmd "$A" --stride 1 --max 200 --audit 25 ;;
  *) echo "usage: [ROM=... MAX=...] $0 {gate1|gate3|gate4}" ;;
esac
