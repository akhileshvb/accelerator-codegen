#!/usr/bin/env python3
"""Plot a `run_compare.py tpch` result directory: TPC-H performance-test metrics relative to DuckDB.

(a) Power@Size, Throughput@Size and QphH@Size of each system over DuckDB's (median over paired runs, min..max).
(b) Power test, per accelerator: DuckDB's query interval over the accelerator's, and RF1 / RF2.  Power ratio = geomean
    of these 24 bars (exact when no query interval was raised to max/1000, spec 5.4.1.4).
(c) Throughput test: per accelerator, its summed time over the S query streams; then the query streams' end, the
    refresh stream's end and Ts = the later of the two.  Throughput ratio = the Ts bar.
(d) QphH: each accelerator's factor of the QphH ratio and the refresh functions' (tpch_breakdown.py); they multiply
    to the QphH ratio.  The throughput part is attributed to what bounds Ts on each side.

  python3 benchmarks/compare/plot_tpch_perf.py <out_dir> [--systems "cpp verus"]
Writes <out_dir>/tpch_perf.{png,pdf}.
"""
import argparse
import glob
import json
import math
import os
import statistics
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.ticker import FuncFormatter, NullFormatter  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tpch_breakdown import breakdown, load  # noqa: E402

LABEL = {"cpp": "C++", "verus": "Verus", "verus_cpp": "Verus (C++ store)", "stateless": "Stateless"}
COLOR = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]  # categorical slots 1..4, fixed order
INK, MUTED, GRID = "#1f1f1e", "#6b6a64", "#e4e3de"


def fmt_x(v, _=None):
    return f"{v:g}x"


def style(ax):
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_x))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.grid(axis="y", color=GRID, lw=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(MUTED)
    ax.tick_params(colors=MUTED, labelcolor=INK)
    ax.axhline(1, color=INK, lw=0.8, zorder=3)


def bars_from_one(ax, x, vals, w, color, label=None):
    """Bars that start at 1x (DuckDB), up for a gain, down for a loss."""
    vals = np.asarray(vals, dtype=float)
    ax.bar(x, vals - 1, bottom=1, width=w, color=color, label=label, edgecolor="white", linewidth=1, zorder=2)


def label_bar(ax, x, v, size=6.5):
    text = f"{v:.0f}x" if v >= 10 else f"{v:.3g}x" if 1 <= v < 10 else f"{v:.2g}x"
    ax.annotate(text, (x, v), xytext=(0, 2 if v >= 1 else -2), textcoords="offset points", ha="center",
                va="bottom" if v >= 1 else "top", fontsize=size, color=INK)


def collect(base, acc):
    """Medians over the paired runs of everything the panels show."""
    n = min(len(base), len(acc))
    B, X = base[:n], acc[:n]
    med = statistics.median
    runs = [breakdown(b, x) for b, x in zip(B, X)]
    rf = lambda r, kind: sum(e["us"] for e in r["power_refresh"] if e["kind"] == kind)  # noqa: E731
    qsum = lambda r, q: sum(s[q] for s in r["stream_query_us"])  # noqa: E731
    return {
        "n": n,
        "summary": {m: [x[key] / b[key] for b, x in zip(B, X)]
                    for m, key in [("Power@Size", "power_at_size"), ("Throughput@Size", "throughput_at_size"),
                                   ("QphH@Size", "qphh_at_size")]},
        "power": [med(b["power_query_us"][q] / x["power_query_us"][q] for b, x in zip(B, X)) for q in range(22)]
                 + [med(rf(b, k) / rf(x, k) for b, x in zip(B, X)) for k in ("rf1", "rf2")],
        "tput": [med(qsum(b, q) / qsum(x, q) for b, x in zip(B, X)) for q in range(22)]
                + [med(b[k] / x[k] for b, x in zip(B, X)) for k in ("query_streams_s", "refresh_stream_s", "ts_s")],
        "bound": ["refresh" if x["refresh_stream_s"] >= x["query_streams_s"] else "queries" for x in X],
        "qphh": [med(math.exp(r[0][q] + r[1][q]) for r in runs) for q in range(22)]
                + [med(math.exp(r[2] + r[3]) for r in runs)],
    }


def per_query_panel(ax, systems, data, key, labels, xs0, w, title, note, ylim, label_idx):
    style(ax)
    k = len(systems)
    for i, s in enumerate(systems):
        xs = xs0 + (i - (k - 1) / 2) * w
        vals = data[s][key]
        bars_from_one(ax, xs, vals, w, COLOR[i], LABEL.get(s, s))
        for j in label_idx(vals):
            label_bar(ax, xs[j], vals[j])
    ax.set_xticks(xs0, labels, fontsize=8)
    ax.set_xlim(xs0[0] - 0.7, xs0[-1] + 0.7)
    ax.set_ylim(*ylim)
    ax.set_title(title, loc="left", fontsize=9.5, color=INK, pad=15)
    ax.text(0, 1.015, note, transform=ax.transAxes, ha="left", va="bottom", fontsize=7.5, color=MUTED)
    ax.set_ylabel("DuckDB time / accelerator time")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--systems", default="cpp verus")
    ap.add_argument("--title", default=None)
    a = ap.parse_args()

    base = load(a.out, "base")
    assert base, "no DuckDB (base) runs"
    data = {s: collect(base, acc) for s in a.systems.split() if (acc := load(a.out, s))}
    systems = list(data)
    assert systems, "no accelerator runs"
    k, w = len(systems), 0.8 / len(systems)
    n = min(d["n"] for d in data.values())
    sf = json.load(open(glob.glob(os.path.join(a.out, "tpch_base_r*.json"))[0]))["sf"]

    plt.rcParams.update({"font.size": 8, "pdf.fonttype": 42, "ps.fonttype": 42, "text.color": INK,
                         "axes.labelcolor": INK})
    fig = plt.figure(figsize=(11, 13), facecolor="white")
    gs = fig.add_gridspec(4, 1, height_ratios=[1.1, 1, 1, 1], hspace=0.62)

    # (a) the three metrics
    ax = fig.add_subplot(gs[0])
    style(ax)
    metrics = ["Power@Size", "Throughput@Size", "QphH@Size"]
    for i, s in enumerate(systems):
        xs = np.arange(3) + (i - (k - 1) / 2) * w
        vals = [data[s]["summary"][m] for m in metrics]
        meds = [statistics.median(v) for v in vals]
        bars_from_one(ax, xs, meds, w, COLOR[i], LABEL.get(s, s))
        ax.errorbar(xs, meds, yerr=[[m - min(v) for m, v in zip(meds, vals)], [max(v) - m for m, v in zip(meds, vals)]],
                    fmt="none", ecolor=INK, elinewidth=0.8, capsize=2, zorder=4)
        for x, v, vs in zip(xs, meds, vals):
            ax.annotate(f"{v:.3g}x", (x, max(vs) if v >= 1 else min(vs)), xytext=(0, 3 if v >= 1 else -3),
                        textcoords="offset points", ha="center", va="bottom" if v >= 1 else "top", fontsize=8.5)
    ax.set_xticks(np.arange(3), metrics, fontsize=9.5)
    ax.set_ylim(0.1, 1000)
    ax.set_ylabel("ratio over DuckDB")
    ax.set_title(a.title or f"(a) TPC-H performance test at SF{sf:g}, ratio over DuckDB "
                 f"(median of {n} runs, whiskers min-max; 1x = DuckDB)", loc="left", fontsize=10.5)
    ax.legend(frameon=False, loc="upper right", ncol=k, fontsize=9)

    qlabels = [f"Q{q}" for q in range(1, 23)]
    gap = lambda m: np.concatenate([np.arange(22.0), 23 + 1.3 * np.arange(m)])  # noqa: E731

    # (b) power test
    per_query_panel(fig.add_subplot(gs[1]), systems, data, "power", qlabels + ["RF1", "RF2"], gap(2), w,
                    "(b) Power test: per-accelerator query speedup, and the refresh functions",
                    "Power@Size ratio = geometric mean of these 24 bars: " + ",  ".join(
                        f"{LABEL.get(s, s)} {statistics.median(data[s]['summary']['Power@Size']):.3g}x" for s in systems),
                    (0.0005, 20000), lambda v: [j for j, x in enumerate(v) if x < 10 or j >= 22])

    # (c) throughput test
    bound = "; ".join(f"{LABEL.get(s, s)} bound by the {max(set(data[s]['bound']), key=data[s]['bound'].count)}"
                      for s in systems)
    per_query_panel(fig.add_subplot(gs[2]), systems, data, "tput", qlabels + ["queries", "refresh", "Ts"],
                    gap(3), w, "(c) Throughput test: per-accelerator time summed over the query streams; stream ends and Ts",
                    f"Throughput@Size ratio = the Ts bar (Ts = later of the query streams' and refresh stream's end; "
                    f"{bound}, DuckDB by its queries)",
                    (0.01, 20000), lambda v: [j for j, x in enumerate(v) if j >= 22])

    # (d) QphH factors
    per_query_panel(fig.add_subplot(gs[3]), systems, data, "qphh", qlabels + ["RF1+RF2"], gap(1), w,
                    "(d) QphH@Size: each accelerator's factor of the QphH ratio, and the refresh functions'",
                    "factors multiply to the QphH ratio: " + ",  ".join(
                        f"{LABEL.get(s, s)} {statistics.median(data[s]['summary']['QphH@Size']):.3g}x" for s in systems)
                    + "   (query q: (power speedup)^(1/48) x its attributed share of the throughput test)",
                    (0.07, 2.5), lambda v: [j for j, x in enumerate(v) if j >= 22])
    fig.axes[-1].set_yticks([0.1, 0.2, 0.5, 1, 1.5, 2])
    fig.axes[-1].set_ylabel("factor over DuckDB")

    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(a.out, f"tpch_perf.{ext}"), dpi=200, bbox_inches="tight")
    print(os.path.join(a.out, "tpch_perf.png"))


if __name__ == "__main__":
    main()
