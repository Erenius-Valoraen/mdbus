#!/usr/bin/env python3
"""Compare variants, and optionally two environments, without clipping.

    python plots/plot_variants.py results/variants

Reads <tag>.<variant>.csv. If more than one tag is present, each variant is
drawn once per tag so the environment effect and the code effect can be read
off the same figure.

Nothing here clips to a percentile. The whole point is to see how far the
outliers actually go, so the axes are logarithmic and the extremes are labelled
rather than hidden.
"""

import argparse
import pathlib
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

INK, GRID, ACCENT = "#1c1c1c", "#dcdcdc", "#7a7a7a"
WARMUP = 0.10
# One colour per variant, one line style per environment.
COLOURS = ["#8d8d8d", "#3a6ea5", "#4c8b5b", "#7a9c3e", "#b8873a", "#c1502e", "#8a4f7d"]
STYLES = {0: "-", 1: "--", 2: ":"}


def load(d: pathlib.Path):
    """-> {tag: {variant: samples}} preserving the order files were named in."""
    out = defaultdict(dict)
    for f in sorted(d.glob("*.csv")):
        if "." not in f.stem:
            continue
        tag, variant = f.stem.split(".", 1)
        s = np.loadtxt(f)
        out[tag][variant.replace("_", " ")] = s[int(len(s) * WARMUP):]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=pathlib.Path)
    ap.add_argument("-o", "--out", type=pathlib.Path, default=None)
    args = ap.parse_args()

    data = load(args.dir)
    if not data:
        raise SystemExit(f"no <tag>.<variant>.csv files in {args.dir}")
    out = args.out or (args.dir / "variants.png")
    tags = list(data)
    variants = list(data[tags[0]])

    plt.rcParams.update({
        "figure.facecolor": "white", "axes.facecolor": "white",
        "axes.edgecolor": ACCENT, "axes.labelcolor": INK, "text.color": INK,
        "xtick.color": INK, "ytick.color": INK, "grid.color": GRID,
        "axes.grid": True, "grid.linewidth": 0.6, "font.size": 10,
    })
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6),
                                   gridspec_kw={"width_ratios": [1.25, 1]})

    # ---- left: full percentile curve, nothing clipped ----------------------
    x = np.logspace(np.log10(2), np.log10(2_000_000), 600)
    ranks = 1.0 - 1.0 / x
    for ti, tag in enumerate(tags):
        for vi, v in enumerate(variants):
            s = data[tag].get(v)
            if s is None:
                continue
            ss = np.sort(s)
            idx = np.clip((ranks * len(ss)).astype(int), 0, len(ss) - 1)
            ax1.plot(x, ss[idx], color=COLOURS[vi % len(COLOURS)],
                     ls=STYLES.get(ti, "-"), lw=1.5,
                     label=f"{v}" if ti == 0 else None, alpha=0.9)
            # mark where each series actually ends
            ax1.plot([len(ss)], [ss[-1]], "o", ms=4,
                     color=COLOURS[vi % len(COLOURS)], alpha=0.8)

    ax1.set_xscale("log"); ax1.set_yscale("log")
    ax1.set_xlim(2, 2_000_000)
    ticks = [2, 10, 100, 1_000, 10_000, 100_000, 1_000_000]
    ax1.set_xticks(ticks)
    ax1.set_xticklabels(["p50", "p90", "p99", "p99.9", "p99.99", "p99.999", "max"])
    ax1.set_xlabel("percentile (each additional nine gets equal width)")
    ax1.set_ylabel("latency (ns)")
    title = "full distribution, nothing clipped"
    if len(tags) > 1:
        title += "   (" + ", ".join(
            f"{t} = {STYLES.get(i, '-')}" for i, t in enumerate(tags)) + ")"
    ax1.set_title(title)
    ax1.legend(frameon=False, fontsize=8.5, ncol=2, loc="upper left")

    # ---- right: how much mass lives past each threshold --------------------
    thresholds = [200, 500, 1_000, 5_000, 10_000, 50_000]
    width = 0.8 / (len(tags) * 1.0)
    pos = np.arange(len(thresholds))
    for ti, tag in enumerate(tags):
        # pool every variant for this environment: the question here is what the
        # machine allows, not which variant is fastest
        pooled = np.concatenate([v for v in data[tag].values()])
        frac = [100.0 * (pooled > t).mean() for t in thresholds]
        bars = ax2.bar(pos + ti * width, np.maximum(frac, 1e-5), width * 0.92,
                       label=tag, color=COLOURS[(ti * 4 + 1) % len(COLOURS)],
                       edgecolor="none")
        for b, f in zip(bars, frac):
            if f > 0:
                ax2.text(b.get_x() + b.get_width() / 2, f * 1.35,
                         f"{f:.3g}%" if f >= 0.001 else "<0.001%",
                         ha="center", fontsize=7.5, color=INK, rotation=90)
    ax2.set_yscale("log")
    ax2.set_xticks(pos + width * (len(tags) - 1) / 2)
    ax2.set_xticklabels([f"> {t / 1000:g} us" if t >= 1000 else f"> {t} ns"
                         for t in thresholds])
    ax2.set_ylabel("share of messages (%)")
    ax2.set_title("how much of the distribution lives past each threshold")
    ax2.legend(frameon=False, fontsize=9)

    fig.suptitle(
        f"{len(variants)} code variants"
        + (f" x {len(tags)} environments" if len(tags) > 1 else "")
        + f", {sum(len(s) for t in data.values() for s in t.values()):,} samples",
        fontsize=11.5, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")

    # ---- the numbers the figure cannot show precisely ----------------------
    print()
    hdr = f"{'':<28}" + "".join(f"{c:>10}" for c in ["p99.9", "p99.99", "p99.999", "max"])
    print(hdr); print("-" * len(hdr))
    for tag in tags:
        for v in variants:
            s = data[tag].get(v)
            if s is None:
                continue
            q = np.percentile(s, [99.9, 99.99, 99.999])
            print(f"{tag + ' / ' + v:<28}" + "".join(
                f"{x:>10,.0f}" for x in [*q, s.max()]))
        print()


if __name__ == "__main__":
    main()
