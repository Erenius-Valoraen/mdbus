#!/usr/bin/env python3
"""Compare several runs of the same benchmark under different conditions.

    python plots/plot_compare.py results/pingpong
    python plots/plot_compare.py a.csv b.csv c.csv -o out.png

Given a directory, groups files named <config>.rep<n>.csv by config and pools
the repetitions. Given files, treats each as its own series. Prints a table and
writes a two-panel figure.

Absolute nanoseconds are not comparable across configurations that change the
clock, so the table also reports p99/p50, which is.
"""

import argparse
import pathlib
import re
import sys
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PALETTE = ["#3a6ea5", "#c1502e", "#4c8b5b", "#8a6fa8", "#b8873a"]
INK, GRID, ACCENT = "#1c1c1c", "#d8d8d8", "#7a7a7a"
WARMUP = 0.10


def read_csv(path):
    first = path.open().readline().strip()
    skip = 1 if any(c.isalpha() for c in first) else 0
    if first.count(",") >= 1:
        return np.loadtxt(path, delimiter=",", skiprows=skip, usecols=1)
    return np.loadtxt(path, skiprows=skip)


def collect(targets):
    """-> {config name: pooled samples}, warmup already dropped per file."""
    groups = defaultdict(list)
    files = []
    for t in targets:
        files.extend(sorted(t.glob("*.csv")) if t.is_dir() else [t])
    if not files:
        sys.exit(f"no CSVs found in {', '.join(str(t) for t in targets)}")
    for f in files:
        m = re.match(r"(.+?)\.rep\d+$", f.stem)
        name = m.group(1) if m else f.stem
        s = read_csv(f)
        groups[name].append(s[int(len(s) * WARMUP):])   # drop warmup per run
    return {k: np.concatenate(v) for k, v in groups.items()}, len(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("targets", nargs="+", type=pathlib.Path)
    ap.add_argument("-o", "--out", type=pathlib.Path, default=None)
    args = ap.parse_args()

    groups, nfiles = collect(args.targets)
    first = args.targets[0]
    out = args.out or ((first if first.is_dir() else first.parent) / "compare.png")

    # ---- table -------------------------------------------------------------
    cols = ["min", "p50", "p90", "p99", "p99.9", "p99.99", "max", "p99/p50"]
    print(f"{'config':<18}" + "".join(f"{c:>11}" for c in cols) + f"{'n':>12}")
    print("-" * (18 + 11 * len(cols) + 12))
    base_p50 = None
    for i, (name, s) in enumerate(groups.items()):
        q = [s.min(), *np.percentile(s, [50, 90, 99, 99.9, 99.99]), s.max()]
        ratio = q[3] / q[1]
        if base_p50 is None:
            base_p50 = q[1]
        print(f"{name:<18}" + "".join(f"{v:>11,.0f}" for v in q)
              + f"{ratio:>11.2f}" + f"{len(s):>12,}")
    print()
    print("p99/p50 is the column to compare across configurations: turning turbo")
    print("off changes the clock, so absolute nanoseconds are not comparable, but")
    print("the shape of the distribution is.")

    # ---- figure ------------------------------------------------------------
    plt.rcParams.update({
        "figure.facecolor": "white", "axes.facecolor": "white",
        "axes.edgecolor": ACCENT, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK, "ytick.color": INK, "grid.color": GRID,
        "axes.grid": True, "grid.linewidth": 0.6, "font.size": 10,
    })
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

    x = np.logspace(np.log10(2), np.log10(100_000), 400)
    ranks = 1.0 - 1.0 / x
    for i, (name, s) in enumerate(groups.items()):
        c = PALETTE[i % len(PALETTE)]
        ss = np.sort(s)
        idx = np.clip((ranks * len(ss)).astype(int), 0, len(ss) - 1)
        ax1.plot(x, ss[idx], color=c, lw=1.6,
                 label=f"{name}  (p50 {np.median(s):,.0f} ns)")

        # Body only: a shared linear axis would be unreadable if one config sits
        # at twice the latency of another, so each is normalised to its own
        # median and the comparison becomes about shape.
        body = s[s < np.percentile(s, 99)] / np.median(s)
        ax2.hist(body, bins=180, range=(0.5, 2.2), histtype="step",
                 color=c, lw=1.4, density=True, label=name)

    ax1.set_xscale("log"); ax1.set_yscale("log")
    ax1.set_xlim(2, 100_000)
    ax1.set_xticks([2, 10, 100, 1_000, 10_000, 100_000])
    ax1.set_xticklabels(["p50", "p90", "p99", "p99.9", "p99.99", "p99.999"])
    ax1.set_xlabel("percentile"); ax1.set_ylabel("latency (ns)")
    ax1.set_title("latency by percentile, absolute")
    ax1.legend(frameon=False, fontsize=9)

    ax2.axvline(1.0, color=ACCENT, lw=1.0, ls="--")
    ax2.set_xlabel("latency / that config's median")
    ax2.set_ylabel("density")
    ax2.set_title("shape of the body, normalised to each median")
    ax2.legend(frameon=False, fontsize=9)

    fig.suptitle(f"{len(groups)} configurations, {nfiles} runs, "
                 f"first {WARMUP:.0%} of each discarded", fontsize=11, y=0.99)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=150)
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
