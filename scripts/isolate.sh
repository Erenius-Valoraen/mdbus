#!/usr/bin/env bash
#
# Isolate CPUs at runtime, without a reboot and without kernel command line
# changes.
#
#   sudo ./scripts/isolate.sh on  [cpus]     default 4,6
#   sudo ./scripts/isolate.sh off
#        ./scripts/isolate.sh status
#   sudo ./scripts/isolate.sh run <cmd...>   isolate, run, restore
#
# Three separate mechanisms, in increasing order of how much they help:
#
#  1. cgroup v2 isolated partition. Writing "isolated" to cpuset.cpus.partition
#     removes the CPUs from the scheduler's load balancing domains and from the
#     root cgroup's effective set, so nothing else gets scheduled there. This is
#     what isolcpus does, except it can be undone without rebooting.
#
#  2. IRQ affinity. Device interrupts are steered to CPUs by an affinity mask
#     per IRQ; by default that includes every CPU. An interrupt landing on a
#     benchmark core costs a few microseconds and lands in the tail. Not every
#     IRQ can be moved: per-CPU interrupts (the local timer, IPIs, and anything
#     the driver pins) will refuse, and those refusals are expected.
#
#  3. SCHED_FIFO. A real-time priority means the kernel will not preempt the
#     thread in favour of a normal task. Combined with 1 and 2 this leaves
#     little besides the local timer tick, which genuinely does need nohz_full
#     on the kernel command line.
#
# What this does NOT replace: nohz_full. The periodic scheduler tick still
# fires on isolated CPUs at up to 1 kHz. That one needs a boot parameter.

set -euo pipefail

CG=/sys/fs/cgroup
BENCH=$CG/bench
STATE=/var/tmp/isolate.state
DEFAULT_CPUS=4,6

need_root() { [ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }; }

expand() {  # "4,6" or "4-6" -> "4 6"
    local out=() part lo hi
    IFS=, read -ra parts <<< "$1"
    for part in "${parts[@]}"; do
        if [[ $part == *-* ]]; then
            lo=${part%-*}; hi=${part#*-}
            for ((n = lo; n <= hi; n++)); do out+=("$n"); done
        else out+=("$part"); fi
    done
    echo "${out[@]}"
}

status() {
    echo "runtime isolation"
    if [ -d "$BENCH" ]; then
        echo "  cgroup          $BENCH exists"
        echo "    cpus          $(cat $BENCH/cpuset.cpus 2>/dev/null)"
        echo "    partition     $(cat $BENCH/cpuset.cpus.partition 2>/dev/null)"
        echo "    effective     $(cat $BENCH/cpuset.cpus.effective 2>/dev/null)"
        echo "    tasks         $(wc -l < $BENCH/cgroup.procs 2>/dev/null || echo 0)"
    else
        echo "  cgroup          not created"
    fi
    echo "  root effective  $(cat $CG/cpuset.cpus.effective 2>/dev/null || echo '(cpuset not enabled)')"
    echo "  kernel isolcpus $(cat /sys/devices/system/cpu/isolated)  (boot parameter, separate)"
    echo "  kernel nohz_full $(cat /sys/devices/system/cpu/nohz_full)  (boot parameter, separate)"
    echo
    echo "interrupts taken, per CPU (since boot)"
    awk 'NR==1{for(i=1;i<=NF;i++) h[i]=$i; next}
         {for(i=2;i<=NF-0;i++) if ($i ~ /^[0-9]+$/) t[i]+=$i}
         END{for(i=2;i<=20+1;i++) if (t[i]) printf "  cpu%-3d %12d\n", i-2, t[i]}' /proc/interrupts
}

apply() {
    need_root on
    local cpus=${1:-$DEFAULT_CPUS}
    [ -f "$STATE" ] && { echo "already applied; run '$0 off' first" >&2; exit 1; }

    echo "cpus=$cpus" > "$STATE"

    # 1. cpuset controller has to be enabled in the root before a child can use it
    if ! grep -qw cpuset $CG/cgroup.subtree_control 2>/dev/null; then
        echo "+cpuset" > $CG/cgroup.subtree_control
    fi
    mkdir -p "$BENCH"
    echo "$cpus" > $BENCH/cpuset.cpus
    if echo isolated > $BENCH/cpuset.cpus.partition 2>/dev/null; then
        echo "  isolated partition: $cpus  (removed from load balancing)"
    else
        echo "  WARNING: kernel refused an isolated partition; falling back to a plain cpuset"
        echo root > $BENCH/cpuset.cpus.partition 2>/dev/null || true
    fi
    echo "  root cgroup now sees: $(cat $CG/cpuset.cpus.effective)"

    # 2. steer interrupts away. Save every mask we change so it can go back.
    local moved=0 refused=0 mask_all other
    other=$(cat $CG/cpuset.cpus.effective)
    for d in /proc/irq/[0-9]*; do
        [ -f "$d/smp_affinity_list" ] || continue
        local irq cur
        irq=${d##*/}
        cur=$(cat "$d/smp_affinity_list" 2>/dev/null) || continue
        echo "irq:$irq:$cur" >> "$STATE"
        if echo "$other" > "$d/smp_affinity_list" 2>/dev/null; then
            moved=$((moved + 1))
        else
            refused=$((refused + 1))
        fi
    done
    echo "  interrupts: $moved steered away, $refused pinned by the kernel (expected)"
    echo
    echo "isolation on. run '$0 off' to undo."
}

restore() {
    need_root off
    [ -f "$STATE" ] || { echo "nothing to restore" >&2; exit 1; }

    local restored=0
    while IFS=: read -r kind irq mask; do
        [ "$kind" = irq ] || continue
        echo "$mask" > /proc/irq/$irq/smp_affinity_list 2>/dev/null && restored=$((restored + 1)) || true
    done < "$STATE"
    echo "  interrupt affinity restored on $restored IRQs"

    if [ -d "$BENCH" ]; then
        echo member > $BENCH/cpuset.cpus.partition 2>/dev/null || true
        rmdir "$BENCH" 2>/dev/null || echo "  (cgroup busy, remove $BENCH by hand)"
        echo "  cgroup removed"
    fi
    rm -f "$STATE"
    echo "  root cgroup sees: $(cat $CG/cpuset.cpus.effective 2>/dev/null || echo all)"
}

run_in() {
    need_root run
    local cpus=$DEFAULT_CPUS
    apply "$cpus"
    trap 'restore >/dev/null 2>&1 || true' EXIT INT TERM
    echo
    echo "running: $*"
    echo "-------------------------------------------------------------"
    # Join the cgroup, then drop back to the invoking user, at real-time
    # priority. SCHED_FIFO 80 is below the kernel's own threads (>=90) so a
    # runaway spin loop cannot lock the machine up.
    echo $$ > $BENCH/cgroup.procs
    chrt -f 80 sudo -u "${SUDO_USER:-root}" -- chrt -f 80 "$@" || true
    echo "-------------------------------------------------------------"
}

cmd=${1:-status}; shift || true
case "$cmd" in
    on|apply)    apply "${1:-}" ;;
    off|restore) restore ;;
    status)      status ;;
    run)         run_in "$@" ;;
    *) echo "usage: $0 {on [cpus]|off|status|run <cmd...>}" >&2; exit 2 ;;
esac
