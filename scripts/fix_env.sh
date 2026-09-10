#!/usr/bin/env bash
#
# Put the machine into the best state for latency measurement that is
# achievable without rebooting, run something, and put everything back.
#
#   sudo ./scripts/fix_env.sh                        show what it would do
#   sudo ./scripts/fix_env.sh run <command...>       apply, run, restore
#   sudo ./scripts/fix_env.sh permanent              print the kernel cmdline
#                                                    edit for the rest
#
# Everything here is reversible and none of it survives a reboot, which is
# deliberate: a benchmark harness should not quietly reconfigure a machine you
# also use for other things.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CPUS=${BENCH_CPUS:-4,6}
CG=/sys/fs/cgroup
BENCH=$CG/bench
STATE=/var/tmp/fix_env.state

need_root() { [ "$(id -u)" -eq 0 ] || { echo "needs root: sudo $0 $*" >&2; exit 1; }; }
say() { printf '\033[1;36m%s\033[0m\n' "$*"; }

apply() {
    : > "$STATE"
    echo "cpus=$CPUS" >> "$STATE"

    say "1. governor and energy-performance preference"
    # Only online CPUs accept these writes; offline ones return EBUSY.
    local n f ok=0
    for n in $(ls -d /sys/devices/system/cpu/cpu[0-9]* | grep -o '[0-9]*$' | sort -n); do
        [ -f /sys/devices/system/cpu/cpu$n/online ] &&
            [ "$(cat /sys/devices/system/cpu/cpu$n/online)" != 1 ] && continue
        for f in scaling_governor energy_performance_preference; do
            local p=/sys/devices/system/cpu/cpu$n/cpufreq/$f
            [ -w "$p" ] || continue
            echo "$f:$n:$(cat "$p")" >> "$STATE"
            echo performance > "$p" 2>/dev/null && ok=$((ok+1)) || true
        done
    done
    echo "   set on $ok files"

    say "2. isolated cpuset partition on CPUs $CPUS"
    # Writing "isolated" removes these CPUs from the scheduler's load balancing
    # domains and from the root cgroup's effective set, so nothing else runs
    # there. This is what isolcpus does, except it can be undone.
    grep -qw cpuset $CG/cgroup.subtree_control 2>/dev/null || echo "+cpuset" > $CG/cgroup.subtree_control
    mkdir -p "$BENCH"
    echo "$CPUS" > $BENCH/cpuset.cpus
    if echo isolated > $BENCH/cpuset.cpus.partition 2>/dev/null; then
        echo "   isolated; root cgroup now sees $(cat $CG/cpuset.cpus.effective)"
    else
        echo "   kernel refused an isolated partition, using a plain cpuset"
        echo root > $BENCH/cpuset.cpus.partition 2>/dev/null || true
    fi

    say "3. steering interrupts away from CPUs $CPUS"
    # Each IRQ has a CPU mask. Per-CPU interrupts (the local APIC timer, IPIs)
    # and anything a driver pinned will refuse, which is expected.
    local other moved=0 refused=0 irq cur
    other=$(cat $CG/cpuset.cpus.effective)
    for d in /proc/irq/[0-9]*; do
        [ -f "$d/smp_affinity_list" ] || continue
        irq=${d##*/}
        cur=$(cat "$d/smp_affinity_list" 2>/dev/null) || continue
        echo "irq:$irq:$cur" >> "$STATE"
        if echo "$other" > "$d/smp_affinity_list" 2>/dev/null
        then moved=$((moved+1)); else refused=$((refused+1)); fi
    done
    echo "   $moved steered, $refused pinned by the kernel (expected)"

    say "4. deferring what can be deferred"
    # Ask the kernel to flush its writeback and RCU work on other CPUs where
    # the knobs exist. Best effort: not every kernel exposes these.
    for f in /sys/bus/workqueue/devices/writeback/cpumask; do
        [ -w "$f" ] || continue
        echo "wq:$f:$(cat "$f")" >> "$STATE"
        printf '%x' $(( (1 << 20) - 1 - (1 << 4) - (1 << 6) )) > "$f" 2>/dev/null || true
    done
    if [ -w /proc/sys/kernel/watchdog_cpumask ]; then
        echo "watchdog::$(cat /proc/sys/kernel/watchdog_cpumask)" >> "$STATE"
        echo "$other" > /proc/sys/kernel/watchdog_cpumask 2>/dev/null || true
        echo "   soft lockup watchdog moved off $CPUS"
    fi
    echo
}

restore() {
    [ -f "$STATE" ] || return 0
    local kind key val restored=0 nirq
    nirq=$(grep -c '^irq:' "$STATE" || true)
    while IFS=: read -r kind key val; do
        case "$kind" in
            scaling_governor|energy_performance_preference)
                echo "$val" > /sys/devices/system/cpu/cpu$key/cpufreq/$kind 2>/dev/null && restored=$((restored+1)) || true ;;
            irq) echo "$val" > /proc/irq/$key/smp_affinity_list 2>/dev/null || true ;;
            wq)  echo "$val" > "$key" 2>/dev/null || true ;;
            watchdog) echo "$val" > /proc/sys/kernel/watchdog_cpumask 2>/dev/null || true ;;
        esac
    done < "$STATE"
    [ -d "$BENCH" ] && { echo member > $BENCH/cpuset.cpus.partition 2>/dev/null || true; rmdir "$BENCH" 2>/dev/null || true; }
    rm -f "$STATE"
    say "restored: governor and EPP, $nirq IRQ masks, cpuset partition removed"
}

show() {
    echo "would isolate CPUs $CPUS and:"
    echo "  - set governor and EPP to performance on all online CPUs"
    echo "  - create an isolated cgroup v2 cpuset partition (runtime isolcpus)"
    echo "  - steer every movable IRQ away from those CPUs"
    echo "  - move the soft lockup watchdog off them"
    echo "  - run the command at SCHED_FIFO 80 inside that partition"
    echo
    echo "current interrupt counts on the target CPUs:"
    awk -v a=4 -v b=6 'NR==1{next} {ta+=$(a+2); tb+=$(b+2)} END{printf "  cpu%d %d   cpu%d %d\n", a, ta, b, tb}' /proc/interrupts
    echo
    echo "run it:        sudo $0 run ./build/pingpong_variants 5"
    echo "the rest:      sudo $0 permanent"
}

permanent() {
    cat <<'EOF'
The one thing that cannot be done at runtime is nohz_full: the periodic
scheduler tick keeps firing on isolated CPUs at up to 1 kHz, and turning it off
is a boot parameter. To add it on this machine (GRUB):

  sudo cp /etc/default/grub /etc/default/grub.backup
  sudo nano /etc/default/grub

change the line to:

  GRUB_CMDLINE_LINUX_DEFAULT='nowatchdog nvme_load=YES loglevel=3 isolcpus=4,6 nohz_full=4,6 rcu_nocbs=4,6'

then:

  sudo grub-mkconfig -o /boot/grub/grub.cfg
  sudo reboot

What each one does:
  isolcpus=4,6   keeps the scheduler from putting anything on those CPUs.
                 Largely redundant with the cpuset partition this script makes,
                 but applies from boot and cannot be undone by accident.
  nohz_full=4,6  stops the periodic timer tick when only one task is runnable
                 on the CPU. This is the part with no runtime equivalent.
  rcu_nocbs=4,6  moves RCU callback processing to other CPUs, so deferred
                 kernel cleanup does not run on the isolated ones.

Cost: two of twenty CPUs stop being available for general work, so parallel
builds lose about 10%. Everything else is unaffected. To undo it, restore the
backup and re-run grub-mkconfig.

Verify after rebooting with:
  cat /sys/devices/system/cpu/isolated /sys/devices/system/cpu/nohz_full
EOF
}

case "${1:-show}" in
    show) show ;;
    run)
        need_root run
        shift
        trap restore EXIT INT TERM
        apply
        say "running: $*"
        echo "------------------------------------------------------------"
        echo $$ > $BENCH/cgroup.procs
        chrt -f 80 sudo -u "${SUDO_USER:-root}" -- chrt -f 80 "$@" || true
        echo "------------------------------------------------------------"
        ;;
    off) need_root off; restore ;;
    permanent) permanent ;;
    *) echo "usage: $0 {show|run <cmd...>|off|permanent}" >&2; exit 2 ;;
esac
