#!/usr/bin/env python3
"""Geomean speedup of the stateless TPC-H accelerators over DuckDB, single-threaded and multi-threaded.

  python3 eval/bench/tpch/plot_stateless_geomean.py [summary.csv] [--title ...] [--png path]

Reads a stateless summary (default results/stateless_duckdb_sf20_summary.csv: per query and thread count, DuckDB vs the
accelerator at the same PRAGMA threads) and plots the geomean over queries of each *_speedup* column: st_speedup
(1 thread, single-threaded kernels) and par_speedup_<T>t (T threads, parallel kernels).  Writes <png> and the .pdf.
"""
import argparse
import csv
import math
import os
import re

ROOT = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("summary", nargs="?", default=os.path.join(ROOT, "results", "stateless_duckdb_sf20_summary.csv"))
    ap.add_argument("--title", default="TPC-H (SF=20): Stateless Speedup over DuckDB")
    ap.add_argument("--png", default=None)
    a = ap.parse_args()

    rows = [r for r in csv.DictReader(open(a.summary)) if re.fullmatch(r"Q\d+", r["query"])]
    cols = [c for c in rows[0] if c.endswith("speedup") or re.fullmatch(r"par_speedup_\d+t", c)]
    bars = []
    for c in cols:
        vals = [float(r[c]) for r in rows if r[c]]
        gm = math.exp(sum(map(math.log, vals)) / len(vals))
        m = re.fullmatch(r"par_speedup_(\d+)t", c)
        bars.append((f"{m.group(1)} threads" if m else "1 thread", gm))

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 8, "pdf.fonttype": 42, "ps.fonttype": 42})
    # one-hue ordinal ramp, light -> dark as the thread count grows
    colors = ["#86b6ef", "#2a78d6", "#104281", "#0d366b"]
    fig, ax = plt.subplots(figsize=(3.5, 2.4))
    xs = range(len(bars))
    ax.bar(xs, [g for _, g in bars], 0.6, color=colors[:len(bars)], linewidth=0)
    for x, (_, g) in zip(xs, bars):
        ax.text(x, g + 0.05, f"{g:.2f}×", ha="center", va="bottom", fontsize=7.5)
    ax.set_xticks(list(xs), [l for l, _ in bars])
    ax.tick_params(axis="x", length=0)
    ax.set_ylim(0, max(g for _, g in bars) * 1.18)
    ax.axhline(1.0, color="#777777", lw=0.8, ls="--")
    ax.set_ylabel("Geomean speedup")
    ax.set_title(a.title, fontsize=8.5)
    ax.grid(axis="y", color="#dddddd", lw=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    fig.tight_layout(pad=0.2)
    png = a.png or os.path.splitext(a.summary)[0].replace("_summary", "") + "_geomean.png"
    fig.savefig(png, dpi=300, bbox_inches="tight", pad_inches=0.03)
    pdf = os.path.splitext(png)[0] + ".pdf"
    fig.savefig(pdf, bbox_inches="tight", pad_inches=0.03)
    print("wrote", png, "and", pdf, "|", ", ".join(f"{l}: {g:.3f}x" for l, g in bars))


if __name__ == "__main__":
    main()
