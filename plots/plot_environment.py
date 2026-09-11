#!/usr/bin/env python3
"""Two runs of the same code, two machine configurations.

    python plots/plot_environment.py results/bus/ordinary.bus.csv results/bus/isolated.bus.csv
"""

import argparse
import pathlib

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK, GRID, ACCENT = "#1c1c1c", "#dcdcdc", "#8a8a8a"
BEFORE, AFTER = "#c1502e", "#3a6ea5"
WARMUP = 0.10


def read(p: pathlib.Path):
    first = p.open().readline().strip()
    skip = 1 if any(c.isalpha() for c in first) else 0
    s = (np.loadtxt(p, delimiter=",", skiprows=skip, usecols=1) if first.count(",")
         else np.loadtxt(p, skiprows=skip))
    s = np.atleast_1d(s)
    return s[int(len(s) * WARMUP):]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before", type=pathlib.Path)
    ap.add_argument("after", type=pathlib.Path)
    ap.add_argument("--labels", nargs=2, default=["ordinary machine", "isolated cores"])
    ap.add_argument("-o", "--out", type=pathlib.Path, required=True)
    args = ap.parse_args()

    a, b = read(args.before), read(args.after)
    la, lb = args.labels

    plt.rcParams.update({
        "figure.facecolor": "white", "axes.facecolor": "white",
        "axes.edgecolor": ACCENT, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK, "ytick.color": INK, "grid.color": GRID,
        "axes.grid": True, "grid.linewidth": 0.6, "font.size": 10.5,
    })
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.4),
                                   gridspec_kw={"width_ratios": [1.3, 1]})

    x = np.logspace(np.log10(2), np.log10(1_000_000), 500)
    ranks = 1.0 - 1.0 / x
    for s, c, lbl in ((a, BEFORE, la), (b, AFTER, lb)):
        ss = np.sort(s)
        idx = np.clip((ranks * len(ss)).astype(int), 0, len(ss) - 1)
        ax1.plot(x, ss[idx], color=c, lw=2.0, label=f"{lbl}   (p50 {np.median(s):,.0f} ns)")
    for xv, lab in [(2, "p50"), (100, "p99"), (1_000, "p99.9"), (10_000, "p99.99")]:
        r = 1 - 1 / xv
        va, vb = np.quantile(a, r), np.quantile(b, r)
        if vb > 0 and va / vb > 1.4:
            ax1.annotate(f"{va/vb:.0f}x", xy=(xv, np.sqrt(va * vb)), ha="center",
                         fontsize=10, color=INK, fontweight="bold",
                         bbox=dict(boxstyle="round,pad=0.28", fc="white", ec=GRID, lw=0.7))
    ax1.set_xscale("log"); ax1.set_yscale("log")
    ax1.set_xlim(2, 1_000_000)
    ax1.set_xticks([2, 10, 100, 1_000, 10_000, 100_000, 1_000_000])
    ax1.set_xticklabels(["p50", "p90", "p99", "p99.9", "p99.99", "p99.999", "max"])
    ax1.set_xlabel("percentile (each additional nine gets equal width)")
    ax1.set_ylabel("latency (ns)")
    ax1.set_title("the same code on two machine configurations")
    ax1.legend(frameon=False, fontsize=10, loc="upper left")

    thresholds = [200, 500, 1_000, 5_000, 10_000, 50_000]
    pos = np.arange(len(thresholds))
    for i, (s, c, lbl) in enumerate(((a, BEFORE, la), (b, AFTER, lb))):
        frac = [100.0 * (s > t).mean() for t in thresholds]
        bars = ax2.bar(pos + i * 0.4, np.maximum(frac, 1e-4), 0.37, label=lbl,
                       color=c, edgecolor="none")
        for bar, f in zip(bars, frac):
            ax2.text(bar.get_x() + bar.get_width() / 2, max(f, 1e-4) * 1.4,
                     f"{f:.3g}" if f >= 1e-3 else "0", ha="center",
                     fontsize=8, color=INK, rotation=90)
    ax2.set_yscale("log")
    ax2.set_xticks(pos + 0.2)
    ax2.set_xticklabels([f"{t//1000} us" if t >= 1000 else f"{t} ns" for t in thresholds])
    ax2.set_xlabel("latency exceeded")
    ax2.set_ylabel("share of messages (%)")
    ax2.set_title("how many messages land past each threshold")
    ax2.legend(frameon=False, fontsize=10)

    fig.suptitle(f"{len(a) + len(b):,} messages, 64 bytes each, paced at 1M/s, "
                 f"producer and consumer on separate physical cores",
                 fontsize=11, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
