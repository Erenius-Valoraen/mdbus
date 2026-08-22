import numpy as np
import matplotlib.pyplot as plt

# ---- load & clean ----
samples = np.loadtxt("samples.csv")
samples = samples[len(samples)//10:]          # drop first 10% (warmup)
samples = samples[samples > 0]                # log axes need positive values
n = len(samples)
x = np.arange(n)

median = np.median(samples)
p99    = np.percentile(samples, 99)
p999   = np.percentile(samples, 99.9)
lo, hi = samples.min(), samples.max()

def mark(ax, vertical):
    """draw median / p99 / p99.9 reference lines"""
    fn = ax.axvline if vertical else ax.axhline
    fn(median, color="orange", lw=1.3, label=f"median = {median:.0f} ns")
    fn(p99,    color="red",    lw=0.9, ls="--", label=f"p99 = {p99:.0f} ns")
    fn(p999,   color="purple", lw=0.9, ls=":",  label=f"p99.9 = {p999:.0f} ns")

fig, axes = plt.subplots(2, 2, figsize=(16, 9))

# ---- top-left: linear histogram, zoomed to the body ----
ax = axes[0, 0]
ax.hist(samples, bins=300, range=(lo, p999))
mark(ax, vertical=True)
ax.set_xlim(lo, p999)
ax.set_title(f"histogram — linear, zoomed to p99.9 ({p999:.0f} ns)")
ax.set_xlabel("RTT (ns)"); ax.set_ylabel("count"); ax.legend()

# ---- bottom-left: log-x histogram, full range incl. tail ----
ax = axes[1, 0]
ax.hist(samples, bins=np.logspace(np.log10(lo), np.log10(hi), 300))
ax.set_xscale("log")
mark(ax, vertical=True)
ax.set_title("histogram — log-x, full range (tail visible)")
ax.set_xlabel("RTT (ns)"); ax.set_ylabel("count"); ax.legend()

# ---- top-right: time-series DENSITY, zoomed to the body ----
ax = axes[0, 1]
hb = ax.hexbin(x, samples, gridsize=250, bins="log", cmap="viridis",
               extent=(0, n, lo, p999))
mark(ax, vertical=False)
ax.set_ylim(lo, p999)
ax.set_title("time series — density, zoomed to p99.9")
ax.set_xlabel("sample #"); ax.set_ylabel("RTT (ns)"); ax.legend(loc="upper right")
fig.colorbar(hb, ax=ax, label="count (log)")

# ---- bottom-right: time-series scatter, log-y, full range ----
ax = axes[1, 1]
ax.plot(x, samples, ".", ms=1, alpha=0.3, rasterized=True)
ax.set_yscale("log")
mark(ax, vertical=False)
ax.set_title("time series — log-y, spikes + quantization bands")
ax.set_xlabel("sample #"); ax.set_ylabel("RTT (ns)"); ax.legend(loc="upper right")

fig.suptitle(f"ping-pong RTT   n={n:,}   median={median:.0f}   "
             f"p99={p99:.0f}   p99.9={p999:.0f}   max={hi:.0f} ns", fontsize=14)
plt.tight_layout()
plt.savefig("latency.png", dpi=170)
print(f"median={median:.1f}  p99={p99:.1f}  p99.9={p999:.1f}  max={hi:.1f}  n={n}")