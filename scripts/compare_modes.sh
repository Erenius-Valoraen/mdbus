#!/usr/bin/env bash
#
# Run the ping-pong under each machine configuration and write one CSV per run,
# so the effect of SMT and turbo can be attributed rather than guessed at.
#
#   sudo ./scripts/compare_modes.sh [reps]
#
# Needs root, because it toggles SMT and turbo between configurations. The
# benchmark itself is run as the invoking user so the CSVs are not left owned by
# root. Whatever the machine state was on entry is restored on exit, including
# if the script is interrupted.
#
# Output: results/pingpong/<config>.rep<n>.csv, then a summary table.

set -euo pipefail

REPS=${1:-3}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BENCHMODE="$ROOT/scripts/benchmode.sh"
BIN="$ROOT/build/pingpong_legacy"
OUT="$ROOT/results/pingpong"
SETTLE=3          # seconds to let frequency and thermals settle after a toggle

# Configurations, in the order they are run. Baseline first so it is measured
# before any toggling has happened.
CONFIGS=(
    "baseline:SMT on, turbo on:"
    "nosmt:SMT off, turbo on:--keep-turbo"
    "nosmt_noturbo:SMT off, turbo off:"
)

[ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }
RUN_AS=${SUDO_USER:-root}

restore_all() {
    if [ -f /var/tmp/benchmode.state ]; then
        echo
        echo "restoring machine state..."
        "$BENCHMODE" off >/dev/null 2>&1 || true
    fi
}
trap restore_all EXIT INT TERM

echo "building..."
sudo -u "$RUN_AS" cmake --build "$ROOT/build" -j >/dev/null
[ -x "$BIN" ] || { echo "no $BIN" >&2; exit 1; }

mkdir -p "$OUT"
chown "$RUN_AS" "$OUT" 2>/dev/null || true

# Start from a known state rather than whatever a previous run left behind.
restore_all

for entry in "${CONFIGS[@]}"; do
    IFS=: read -r name desc args <<< "$entry"
    echo
    echo "=================================================================="
    echo " $name  ($desc)"
    echo "=================================================================="

    if [ -n "$args" ] || [ "$name" != baseline ]; then
        "$BENCHMODE" on $args | sed 's/^/  /'
        echo "  settling ${SETTLE}s..."
        sleep "$SETTLE"
    else
        echo "  (no changes, measuring the machine as you normally use it)"
    fi

    for r in $(seq 1 "$REPS"); do
        f="$OUT/$name.rep$r.csv"
        printf '  rep %d/%d -> %s ' "$r" "$REPS" "${f#$ROOT/}"
        sudo -u "$RUN_AS" "$BIN" "$f" >/dev/null
        printf 'ok (%s samples)\n' "$(wc -l < "$f")"
    done

    [ "$name" != baseline ] && "$BENCHMODE" off >/dev/null
done

echo
echo "=================================================================="
echo " summary"
echo "=================================================================="
sudo -u "$RUN_AS" "$ROOT/.venv/bin/python" "$ROOT/plots/plot_compare.py" "$OUT" \
    || echo "(comparison plot failed; CSVs are still in ${OUT#$ROOT/})"
