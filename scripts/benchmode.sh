#!/usr/bin/env bash
#
# Put the machine into a quieter state for benchmarking, and put it back.
#
#   sudo ./scripts/benchmode.sh on       apply, saving the current state first
#   sudo ./scripts/benchmode.sh off      restore whatever was saved
#        ./scripts/benchmode.sh status   show current state, no root needed
#
# Only touches settings that are reversible without a reboot. Core isolation
# (isolcpus, nohz_full, rcu_nocbs) is a kernel command line matter and is
# deliberately not handled here.
#
# What it changes and what it costs you:
#
#   SMT off    Each physical core stops presenting two logical CPUs, so nothing
#              can be scheduled onto a sibling of a benchmark core and steal its
#              execution units. Costs you 6 of 20 logical CPUs, so parallel
#              builds get roughly 25% slower while it is off.
#
#   Turbo off  Every core is pinned to its base clock, which removes the drift
#              that comes from boost frequency depending on package temperature
#              and how many cores are busy. Costs you a lot of single-thread
#              speed; the whole machine feels slower.
#
#   Governor   Set to performance so the CPU does not sit at a low P-state
#              waiting to be convinced there is work to do.
#
# Turbo off is the one that trades the most away. If your run is short enough
# that the package never heats up, you may not need it; measure with it on and
# off before deciding.

set -euo pipefail

STATE=/var/tmp/benchmode.state
SMT=/sys/devices/system/cpu/smt/control
TURBO=/sys/devices/system/cpu/intel_pstate/no_turbo

have() { [ -e "$1" ]; }
need_root() {
    [ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }
}

read_governors() { cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | paste -sd,; }
read_epp()       { cat /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference 2>/dev/null | sort -u | paste -sd,; }

status() {
    echo "current state"
    have $SMT   && echo "  SMT             $(cat $SMT)"                || echo "  SMT             (not controllable)"
    have $TURBO && echo "  turbo           $([ "$(cat $TURBO)" = 0 ] && echo on || echo off)" \
                || echo "  turbo           (not controllable)"
    echo "  governor        $(read_governors)"
    echo "  EPP             $(read_epp)"
    echo "  online CPUs     $(nproc)"
    echo "  isolated CPUs   $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo -)  (needs a kernel cmdline change)"
    echo "  nohz_full CPUs  $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo -)  (needs a kernel cmdline change)"
    if [ -f "$STATE" ]; then
        echo
        echo "saved state exists ($STATE), so benchmark mode is currently on:"
        sed 's/^/  /' "$STATE"
    fi
}

apply() {
    need_root on
    if [ -f "$STATE" ]; then
        echo "a saved state already exists; run '$0 off' first, or delete $STATE" >&2
        exit 1
    fi

    {
        echo "SMT=$(have $SMT && cat $SMT || echo -)"
        echo "TURBO=$(have $TURBO && cat $TURBO || echo -)"
        echo "GOV=$(read_governors)"
        echo "EPP=$(read_epp)"
    } > "$STATE"
    echo "saved current state to $STATE"

    if have $SMT && [ "$(cat $SMT)" != off ]; then
        echo off > $SMT && echo "  SMT off        ($(nproc) logical CPUs online)"
    fi
    if have $TURBO && [ "$(cat $TURBO)" != 1 ]; then
        echo 1 > $TURBO && echo "  turbo off"
    fi
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -w "$f" ] && echo performance > "$f" || true
    done
    for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
        [ -w "$f" ] && echo performance > "$f" || true
    done
    echo "  governor and EPP set to performance"
    echo
    echo "benchmark mode on. run '$0 off' when you are done."
}

restore() {
    need_root off
    [ -f "$STATE" ] || { echo "nothing saved at $STATE, nothing to restore" >&2; exit 1; }
    # Deliberately not sourced: the file sets SMT= and TURBO=, which are the
    # names this script uses for the sysfs paths.
    saved() { grep "^$1=" "$STATE" | cut -d= -f2-; }

    prev=$(saved SMT)
    if have $SMT && [ -n "$prev" ] && [ "$prev" != - ]; then
        echo "$prev" > $SMT && echo "  SMT restored to $prev"
    fi
    prev=$(saved TURBO)
    if have $TURBO && [ "$prev" != - ]; then
        echo "$prev" > $TURBO && echo "  turbo restored ($([ "$prev" = 0 ] && echo on || echo off))"
    fi
    prev=$(saved GOV | cut -d, -f1)
    if [ -n "$prev" ]; then
        for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            [ -w "$f" ] && echo "$prev" > "$f" || true
        done
        echo "  governor restored to $prev"
    fi
    prev=$(saved EPP | cut -d, -f1)
    if [ -n "$prev" ]; then
        for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
            [ -w "$f" ] && echo "$prev" > "$f" || true
        done
        echo "  EPP restored to $prev"
    fi

    rm -f "$STATE"
    echo
    echo "normal mode restored ($(nproc) logical CPUs online)."
}

case "${1:-status}" in
    on|apply)    apply ;;
    off|restore) restore ;;
    status)      status ;;
    *) echo "usage: $0 {on|off|status}" >&2; exit 2 ;;
esac
