#!/usr/bin/env python3
import json
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib as mpl
import numpy as np

REPO = Path(__file__).resolve().parent.parent
RESULTS = REPO / "results" / "latest.json"
FIGURES = REPO / "docs" / "figures"
FIGURES.mkdir(parents=True, exist_ok=True)

COLOR_ARENA  = "#1f3a5f"
COLOR_MALLOC = "#b8553a"
COLOR_WIN    = "#7a8b6c"
GRID_COLOR   = "#d4d4d4"

mpl.rcParams.update({
    "font.family":      "Arial",
    "font.size":        10,
    "axes.titlesize":   11,
    "axes.labelsize":   10,
    "axes.edgecolor":   "#333",
    "axes.titleweight": "bold",
    "axes.titlecolor":  "#1f3a5f",
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "grid.color":       GRID_COLOR,
    "grid.linewidth":   0.6,
    "figure.dpi":       140,
    "savefig.dpi":      140,
    "savefig.bbox":     "tight",
})

def load():
    if not RESULTS.exists():
        raise SystemExit(f"no results yet — run scripts/run_bench.py first ({RESULTS} missing)")
    return json.loads(RESULTS.read_text())

def plot_throughput(data):
    fig, ax = plt.subplots(figsize=(6.5, 4))
    labels = list(data.keys())
    values = [data[k]["median"]["throughput"] / 1e6 for k in labels]
    colors = [COLOR_ARENA if k == "arena_pool" else COLOR_MALLOC for k in labels]

    bars = ax.bar(labels, values, color=colors, width=0.55, edgecolor="white", linewidth=1.5)
    for bar, v in zip(bars, values):
        ax.text(bar.get_x() + bar.get_width() / 2, v + max(values) * 0.02,
                f"{v:.2f}M/s", ha="center", va="bottom", fontsize=10, color="#333")

    ax.set_ylabel("Throughput (M ops/sec)")
    ax.set_title("Single-thread throughput — arena pool vs malloc baseline")
    ax.set_ylim(0, max(values) * 1.18)
    ax.yaxis.grid(True, linestyle="--", alpha=0.6)
    ax.set_axisbelow(True)

    fig.savefig(FIGURES / "throughput.png")
    print(f"wrote {FIGURES / 'throughput.png'}")

def plot_latency(data):
    fig, ax = plt.subplots(figsize=(7, 4.2))
    percentiles = ["p50_ns", "p90_ns", "p99_ns", "p999_ns"]
    labels      = ["p50",    "p90",    "p99",    "p99.9"]

    x = np.arange(len(percentiles))
    width = 0.36

    arena  = [data["arena_pool"]["median"][p] for p in percentiles]
    malloc = [data["malloc"]["median"][p]     for p in percentiles]

    ax.bar(x - width/2, arena,  width, label="arena pool", color=COLOR_ARENA, edgecolor="white", linewidth=1)
    ax.bar(x + width/2, malloc, width, label="malloc",     color=COLOR_MALLOC, edgecolor="white", linewidth=1)

    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Latency (ns, log scale)")
    ax.set_title("Per-operation latency by percentile")
    ax.legend(frameon=False, loc="upper left")
    ax.yaxis.grid(True, which="both", linestyle="--", alpha=0.6)
    ax.set_axisbelow(True)

    fig.savefig(FIGURES / "latency_percentiles.png")
    print(f"wrote {FIGURES / 'latency_percentiles.png'}")

def plot_relative(data):
    fig, ax = plt.subplots(figsize=(6, 3.8))
    arena  = data["arena_pool"]["median"]
    malloc = data["malloc"]["median"]

    metrics = ["throughput", "p50_ns", "p99_ns", "p999_ns"]
    labels  = ["throughput\n(↑ better)", "p50\n(↓ better)", "p99\n(↓ better)", "p99.9\n(↓ better)"]
    deltas = []
    for m in metrics:
        if m == "throughput":
            deltas.append((arena[m] - malloc[m]) / malloc[m] * 100)
        else:
            deltas.append((malloc[m] - arena[m]) / malloc[m] * 100)

    colors = [COLOR_WIN if d >= 0 else COLOR_MALLOC for d in deltas]
    bars = ax.bar(labels, deltas, color=colors, edgecolor="white", linewidth=1.5, width=0.6)
    for bar, d in zip(bars, deltas):
        offset = max(deltas) * 0.03 if d >= 0 else min(deltas) * 0.08
        ax.text(bar.get_x() + bar.get_width() / 2, d + offset,
                f"{d:+.1f}%", ha="center",
                va="bottom" if d >= 0 else "top",
                fontsize=10, color="#333")

    ax.axhline(0, color="#333", linewidth=0.8)
    ax.set_ylabel("Arena pool vs malloc (%)")
    ax.set_title("Improvement from arena allocation")
    ax.yaxis.grid(True, linestyle="--", alpha=0.6)
    ax.set_axisbelow(True)

    fig.savefig(FIGURES / "relative_improvement.png")
    print(f"wrote {FIGURES / 'relative_improvement.png'}")

def main():
    data = load()
    plot_throughput(data)
    plot_latency(data)
    plot_relative(data)
    print("\nall figures rendered to docs/figures/")

if __name__ == "__main__":
    main()
