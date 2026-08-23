#!/usr/bin/env python3
"""Four-panel view of a single benchmark run.

    python plots/plot_run.py results/spsc_padded [-o results/spsc_padded/latency.png]

Reads latency.csv (consumer_id, sample_ns) and run_meta.json from the given
directory. The CSV is written in arrival order, which the time-series panel
needs, so nothing here may sort the array in place before that panel is drawn.
"""

import argparse
import json
import pathlib
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK   = "#1c1c1c"
BODY  = "#3a6ea5"
TAIL  = "#c1502e"
GRID  = "#d8d8d8"
ACCENT= "#7a7a7a"


def load(run_dir: pathlib.Path):
    csv = run_dir / "latency.csv"
    if not csv.exists():
        sys.exit(f"no latency.csv in {run_dir}")
    samples = np.loadtxt(csv, delimiter=",", skiprows=1, usecols=1)
    meta = {}
    meta_path = run_dir / "run_meta.json"
    if meta_path.exists():
        meta = json.loads(meta_path.read_text())
    return samples, meta


def percentile_axis(ax, samples):
    """Latency against percentile, x on a 1/(1-p) log scale.

    This is the plot that actually shows a tail. A plain CDF crushes everything
    above p90 into the last pixel column; stretching the axis by 1/(1-p) gives
    each additional nine the same width, so p99 -> p99.9 -> p99.99 are readable.
    """
    s = np.sort(samples)
    n = len(s)
    # Evaluate on a log-spaced grid of 1/(1-p) rather than every sample: with
    # millions of points most of them land in the first pixel column anyway.
    x = np.logspace(np.log10(2), np.log10(100_000), 400)
    ranks = 1.0 - 1.0 / x
    idx = np.clip((ranks * n).astype(int), 0, n - 1)
    ax.plot(x, s[idx], color=TAIL, lw=1.6)

    for xv, lbl in [(2, "p50"), (10, "p90"), (100, "p99"),
                    (1_000, "p99.9"), (10_000, "p99.99")]:
        yv = s[min(int((1 - 1 / xv) * n), n - 1)]
        ax.plot([xv], [yv], "o", ms=4, color=INK, zorder=5)
        ax.annotate(f"{yv:,.0f}", (xv, yv), textcoords="offset points",
                    xytext=(6, -11), fontsize=8.5, color=INK)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(2, 100_000)
    ax.set_xticks([2, 10, 100, 1_000, 10_000, 100_000])
    ax.set_xticklabels(["p50", "p90", "p99", "p99.9", "p99.99", "p99.999"])
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency (ns)")
    ax.set_title("latency by percentile (each nine gets equal width)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=pathlib.Path)
    ap.add_argument("-o", "--out", type=pathlib.Path, default=None)
    args = ap.parse_args()

    samples, meta = load(args.run_dir)
    out = args.out or (args.run_dir / "latency.png")

    p50, p90, p99, p999 = np.percentile(samples, [50, 90, 99, 99.9])
    lo, hi = samples.min(), samples.max()

    plt.rcParams.update({
        "figure.facecolor": "white", "axes.facecolor": "white",
        "axes.edgecolor": ACCENT, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK, "ytick.color": INK, "grid.color": GRID,
        "axes.grid": True, "grid.linewidth": 0.6, "font.size": 10,
    })
    fig, axes = plt.subplots(2, 2, figsize=(14, 8.5))

    def marks(ax, vertical=True):
        line = ax.axvline if vertical else ax.axhline
        line(p50,  color=ACCENT, lw=1.2, label=f"p50 {p50:,.0f} ns")
        line(p99,  color=TAIL,   lw=1.0, ls="--", label=f"p99 {p99:,.0f} ns")
        line(p999, color=INK,    lw=1.0, ls=":",  label=f"p99.9 {p999:,.0f} ns")

    # body of the distribution, linear
    ax = axes[0, 0]
    ax.hist(samples, bins=250, range=(lo, p99), color=BODY, edgecolor="none")
    marks(ax)
    ax.set_xlim(lo, p99)
    ax.set_title(f"distribution, linear, clipped at p99")
    ax.set_xlabel("latency (ns)"); ax.set_ylabel("count")
    ax.legend(frameon=False, fontsize=9)

    # tail plot
    percentile_axis(axes[0, 1], samples)

    # full range, log x
    ax = axes[1, 0]
    ax.hist(samples, bins=np.logspace(np.log10(max(lo, 1)), np.log10(hi), 250),
            color=BODY, edgecolor="none")
    ax.set_xscale("log")
    ax.set_yscale("log")
    marks(ax)
    ax.set_title("distribution, log-log, full range (tail visible)")
    ax.set_xlabel("latency (ns)"); ax.set_ylabel("count")
    ax.legend(frameon=False, fontsize=9)

    # arrival order, so bursts and drift are visible
    ax = axes[1, 1]
    # A scatter of millions of points is a solid block. Chop the run into
    # windows and plot per-window percentiles instead: drift, warm-up and
    # bursts all show up, and the max line marks where the stalls landed.
    nwin = 400
    win = max(len(samples) // nwin, 1)
    trimmed = samples[: (len(samples) // win) * win].reshape(-1, win)
    centres = (np.arange(trimmed.shape[0]) + 0.5) * win
    ax.plot(centres, np.max(trimmed, axis=1), lw=0.7, color=TAIL,
            alpha=0.75, label="window max")
    ax.plot(centres, np.percentile(trimmed, 99, axis=1), lw=1.0, color=INK,
            alpha=0.8, label="window p99")
    ax.plot(centres, np.percentile(trimmed, 50, axis=1), lw=1.4, color=BODY,
            label="window p50")
    ax.set_yscale("log")
    ax.set_title(f"latency over the run ({win:,} messages per window)")
    ax.set_xlabel("message #"); ax.set_ylabel("latency (ns)")
    ax.legend(frameon=False, fontsize=9, loc="upper right")

    bits = [f"n={len(samples):,}"]
    if meta:
        bits += [
            f"{meta.get('transport','?')}",
            f"cores {meta.get('producer_core','?')}→{meta.get('consumer_core','?')}",
            f"{meta.get('ring_slots','?')} slots",
            f"{meta.get('throughput_mmsg_s',0):.1f} M msg/s",
            f"{'padded' if meta.get('cursors_padded') else 'packed'} cursors",
            f"gaps {meta.get('gaps','?')}, torn {meta.get('torn','?')}",
        ]
    fig.suptitle("  |  ".join(bits), fontsize=11.5, y=0.985)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
