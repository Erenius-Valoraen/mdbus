#!/usr/bin/env python3
"""Four-panel view of a single benchmark run.

    python plots/plot_run.py results/spsc_paced          # a run directory
    python plots/plot_run.py samples.csv -o out.png      # a bare CSV

Given a directory, reads latency.csv (consumer_id, sample_ns) plus run_meta.json.
Given a file, reads it directly and accepts either that two-column form or a
single column of nanoseconds per line, which is what the legacy ping-pong
writes. Sample order is arrival order in both cases, which the time-series panel
needs, so nothing here may sort in place before that panel is drawn.
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


def read_samples(csv: pathlib.Path):
    """Accept either `consumer_id,sample_ns` with a header, or one bare
    nanosecond value per line."""
    first = csv.open().readline().strip()
    has_header = any(c.isalpha() for c in first)
    skip = 1 if has_header else 0
    if first.count(",") + 1 >= 2:
        s = np.loadtxt(csv, delimiter=",", skiprows=skip, usecols=1)
    else:
        s = np.loadtxt(csv, skiprows=skip)
    # A one-row file loads as a 0-d array, which has no len().
    return np.atleast_1d(s)


def load(target: pathlib.Path):
    if target.is_dir():
        csv = target / "latency.csv"
        if not csv.exists():
            sys.exit(f"no latency.csv in {target}")
        meta_path = target / "run_meta.json"
        meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
        return read_samples(csv), meta
    if not target.exists():
        sys.exit(f"no such file: {target}")
    return read_samples(target), {}



def quantum(samples):
    """Smallest gap between distinct observed values.

    Latencies derive from a tick counter, so they are quantised: on this
    machine roughly 0.343 ns. Binning at a width that is not a whole multiple
    of that puts two tick values in some bins and one in their neighbours,
    which renders as a comb and hides the real shape.
    """
    u = np.unique(samples)
    if u.size < 2:
        return 0.0
    d = np.diff(u)
    d = d[d > 0]
    return float(d.min()) if d.size else 0.0


def aligned_bins(lo, hi, q, target=220):
    """Bin edges that are a whole number of quanta wide and sit on quantum
    boundaries, so every bin holds the same number of representable values."""
    if q <= 0 or not np.isfinite(q) or hi <= lo:
        return target
    width = max(q, (hi - lo) / target)
    width = max(1, round(width / q)) * q
    start = np.floor(lo / q) * q - q / 2
    n = int(np.ceil((hi - start) / width)) + 1
    return start + np.arange(n + 1) * width


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
    ap.add_argument("target", type=pathlib.Path,
                    help="a run directory, or a CSV file of samples")
    ap.add_argument("-o", "--out", type=pathlib.Path, default=None)
    ap.add_argument("--hero", action="store_true",
                    help="write a single wide percentile panel instead of the "
                         "four-panel view, for embedding somewhere narrow")
    args = ap.parse_args()

    samples, meta = load(args.target)
    if samples.size == 0:
        sys.exit(f"{args.target} contains no samples")
    if args.out:
        out = args.out
    elif args.target.is_dir():
        out = args.target / "latency.png"
    else:
        out = args.target.with_suffix(".png")

    p50, p90, p99, p999 = np.percentile(samples, [50, 90, 99, 99.9])
    lo, hi = samples.min(), samples.max()
    if hi <= lo:
        # Every sample identical: give the axes a nominal width so matplotlib
        # has something to draw rather than warning about a singular range.
        lo, hi = lo * 0.99 or -1.0, hi * 1.01 or 1.0

    if args.hero:
        # A compact view of the body of the distribution, for embedding
        # somewhere narrow. Deliberately clipped at p99: the far tail on an
        # unisolated machine is dominated by kernel scheduling rather than by
        # the transport, so showing it here would be showing the operating
        # system, not the ring.
        plt.rcParams.update({
            "figure.facecolor": "white", "axes.facecolor": "white",
            "axes.edgecolor": ACCENT, "axes.labelcolor": INK, "text.color": INK,
            "xtick.color": INK, "ytick.color": INK, "grid.color": GRID,
            "axes.grid": True, "grid.linewidth": 0.6, "font.size": 11,
        })
        fig, ax = plt.subplots(figsize=(11, 3.8))

        # Choose the clip point from the data rather than fixing it: take the
        # highest percentile still within 3x the median, so a tight
        # distribution is not drawn with most of the axis empty.
        frac, cut = 95.0, np.percentile(samples, 95)
        for cand in (97.0, 98.0, 99.0, 99.5, 99.9):
            v = np.percentile(samples, cand)
            if v <= 3.0 * p50:
                frac, cut = cand, v
        body = samples[samples <= cut]
        ax.hist(body, bins=260, range=(lo, cut), color=BODY, edgecolor="none")
        ax.axvline(p50, color=INK, lw=1.4)
        ax.annotate(f"median {p50:,.0f} ns", (p50, ax.get_ylim()[1] * 0.92),
                    xytext=(10, 0), textcoords="offset points",
                    fontsize=11, color=INK, va="top")
        ax.axvline(lo, color=ACCENT, lw=1.0, ls="--")
        ax.annotate(f"fastest {lo:,.0f} ns", (lo, ax.get_ylim()[1] * 0.55),
                    xytext=(10, 0), textcoords="offset points",
                    fontsize=9.5, color=ACCENT, va="top")
        ax.set_xlim(lo, max(cut, lo + 1e-9))
        ax.set_yticks([])
        ax.set_xlabel("latency (ns)")
        title = f"{len(samples):,} samples ({frac:.0f}% shown)"
        if meta:
            title = (f"{meta.get('messages',0):,} messages, core "
                     f"{meta.get('producer_core','?')} to {meta.get('consumer_core','?')}, "
                     f"{meta.get('gaps',0)} lost, {meta.get('torn',0)} corrupted "
                     f"({frac:.0f}% of messages shown)")
        ax.set_title(title, fontsize=11, pad=10)
        fig.tight_layout()
        out.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(out, dpi=150)
        print(f"wrote {out}")
        return

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
    q = quantum(samples)
    body_hi = np.percentile(samples, 99)
    ax.hist(samples, bins=aligned_bins(lo, body_hi, q), range=(lo, body_hi),
            color=BODY, edgecolor="none")
    marks(ax)
    ax.set_xlim(lo, max(body_hi, lo + 1e-9))
    ax.set_title("body of the distribution, linear, clipped at p99 "
                 f"(bins aligned to the {q:.3f} ns tick)")
    ax.set_xlabel("latency (ns)"); ax.set_ylabel("count")
    ax.legend(frameon=False, fontsize=9)

    # tail plot
    percentile_axis(axes[0, 1], samples)

    # full range, log x
    ax = axes[1, 0]
    edges = np.logspace(np.log10(max(lo, 1e-9)), np.log10(hi), 160)
    counts, edges = np.histogram(samples, bins=edges)
    centres = np.sqrt(edges[:-1] * edges[1:])
    ax.fill_between(centres, 0.5, np.maximum(counts, 0.5), step="mid",
                    color=BODY, alpha=0.55, linewidth=0)
    ax.step(centres, np.maximum(counts, 0.5), where="mid", color=BODY, lw=1.0)
    # A bin holding one sample is a one-pixel bar on a log axis and reads as
    # empty space. Mark every non-empty bin so sparse tail observations are
    # actually visible.
    nz = counts > 0
    ax.plot(centres[nz], counts[nz], "o", ms=2.6, color=TAIL, zorder=4,
            label="occupied bin")
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_ylim(0.5, max(counts.max() * 1.6, 2))
    marks(ax)
    ax.set_title("full range, log-log (every occupied bin marked)")
    ax.set_xlabel("latency (ns)"); ax.set_ylabel("count")

    # How much actually lives out there, in words rather than pixels.
    n = samples.size
    notes = []
    for thr, lbl in [(1e3, "1 us"), (1e4, "10 us"), (1e5, "100 us")]:
        c = int((samples > thr).sum())
        if c:
            notes.append(f"> {lbl}: {c:,} ({100.0 * c / n:.4f}%)")
    if notes:
        ax.text(0.985, 0.95, "\n".join(notes), transform=ax.transAxes,
                ha="right", va="top", fontsize=8.5, color=INK,
                bbox=dict(boxstyle="round,pad=0.35", fc="white", ec=GRID, lw=0.6))
    ax.legend(frameon=False, fontsize=9, loc="upper left")

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
