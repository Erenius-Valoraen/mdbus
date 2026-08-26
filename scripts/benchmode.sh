#!/usr/bin/env bash
#
# Put the machine into a quieter state for benchmarking, and put it back.
#
#   sudo ./scripts/benchmode.sh on                 apply, saving current state
#   sudo ./scripts/benchmode.sh on --keep-turbo    apply, but leave turbo alone
#   sudo ./scripts/benchmode.sh off                restore whatever was saved
#        ./scripts/benchmode.sh status             show current state, no root
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
# Turbo off is the one that trades the most away, and on this machine it is not
# a free win: measured on the ping-pong it halved the relative spread (p99/p50
# from 2.23 to 1.41) but doubled the median, because base clock is roughly half
# of boost. Use --keep-turbo to isolate what SMT alone is worth, and compare the
# three configurations before settling on one.

set -euo pipefail

STATE=/var/tmp/benchmode.state
SMT=/sys/devices/system/cpu/smt/control
TURBO=/sys/devices/system/cpu/intel_pstate/no_turbo

have() { [ -e "$1" ]; }
need_root() {
    [ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }
}

# Offline CPUs keep their cpufreq directory but reject writes with EBUSY, and
# turning SMT off takes six of them away, so anything that writes per-CPU has to
# ask which are actually online first.
online_cpus() {
    local d n
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
        n=${d##*/cpu}
        case $n in *[!0-9]*) continue ;; esac
        if [ -f "$d/online" ] && [ "$(cat "$d/online")" != 1 ]; then continue; fi
        echo "$n"
    done
}

# Writes $2 into the named cpufreq file of every online CPU. Echoes
# "<written> <failed>" so the caller can report what actually happened rather
# than announcing success it did not verify.
write_cpufreq() {
    local what=$1 val=$2 n f ok=0 bad=0
    for n in $(online_cpus); do
        f=/sys/devices/system/cpu/cpu$n/cpufreq/$what
        [ -e "$f" ] || continue
        if printf '%s\n' "$val" > "$f" 2>/dev/null; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
    done
    echo "$ok $bad"
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
    local keep_turbo=0
    for arg in "${@}"; do
        case "$arg" in
            --keep-turbo) keep_turbo=1 ;;
            "") ;;
            *) echo "unknown option: $arg" >&2; exit 2 ;;
        esac
    done
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
    if [ "$keep_turbo" = 1 ]; then
        echo "  turbo          left as-is ($([ "$(cat $TURBO 2>/dev/null)" = 0 ] && echo on || echo off))"
    elif have $TURBO && [ "$(cat $TURBO)" != 1 ]; then
        echo 1 > $TURBO && echo "  turbo off"
    fi
    report() {  # "<ok> <bad>" -> a line the user can trust
        local ok=${1%% *} bad=${1##* } label=$2
        if [ "$ok" -gt 0 ]; then
            printf '  %-14s performance on %d CPU(s)%s\n' "$label" "$ok" \
                "$([ "$bad" -gt 0 ] && echo ", $bad refused" || echo "")"
        elif [ "$bad" -gt 0 ]; then
            printf '  %-14s could not be set (%d refused)\n' "$label" "$bad"
        else
            printf '  %-14s not available on this system\n' "$label"
        fi
    }
    report "$(write_cpufreq scaling_governor performance)" "governor"
    report "$(write_cpufreq energy_performance_preference performance)" "EPP"
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
    # Governor and EPP go last, after SMT is back on: a CPU that was offlined
    # loses its cpufreq policy and comes back with the system default, so the
    # ones that just returned need setting too.
    prev=$(saved GOV | cut -d, -f1)
    if [ -n "$prev" ]; then
        r=$(write_cpufreq scaling_governor "$prev")
        echo "  governor restored to $prev on ${r%% *} CPU(s)"
    fi
    prev=$(saved EPP | cut -d, -f1)
    if [ -n "$prev" ]; then
        r=$(write_cpufreq energy_performance_preference "$prev")
        echo "  EPP restored to $prev on ${r%% *} CPU(s)"
    fi

    rm -f "$STATE"
    echo
    echo "normal mode restored ($(nproc) logical CPUs online)."
}

cmd=${1:-status}
shift || true
case "$cmd" in
    on|apply)    apply "$@" ;;
    off|restore) restore ;;
    status)      status ;;
    *) echo "usage: $0 {on [--keep-turbo]|off|status}" >&2; exit 2 ;;
esac
