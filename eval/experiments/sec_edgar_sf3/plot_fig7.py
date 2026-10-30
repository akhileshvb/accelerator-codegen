#!/usr/bin/env python3
"""Figure 7: SEC-EDGAR (SF=3) read speedup over the host engine (DuckDB, Postgres), 40 vs 40 cores.

JetStream and JetStream Verified: median over the 5 parameter draws of the geomean speedup over each draw's sets
(data/{cpp,verified}/duckdb/read_q*_{cpp,verus}_r*.json, data/{cpp,verified}/pg/cases.csv).  The two come from separate
sweeps (scripts/drive_*_read.sh).  GenDB: host time / GenDB time, default parameters (data/gendb_times.csv).
Usage: plot_fig7.py [out_dir]   writes sec_edgar_read_speedup_vs_gendb.{pdf,png}
"""
import collections
import csv
import glob
import json
import os
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402
import numpy as np  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from figstyle import geomean  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
QUERIES = [1, 2, 3, 4, 6, 24]
BLUE = "#4C72B0"    # JetStream
PURPLE = "#8172B3"  # JetStream Verified
ORANGE = "#DD8452"  # GenDB


def duckdb_speedup(q, run, system):
    files = glob.glob(os.path.join(DATA, run, "duckdb", f"read_q{q}_{system}_r*.json"))
    return statistics.median(json.load(open(f))["speedup_geomean"] for f in files)


def postgres_speedup(run):
    per_draw = collections.defaultdict(list)
    for r in csv.DictReader(open(os.path.join(DATA, run, "pg", "cases.csv"))):
        if r["base_status"] == "ok" and r["accel_status"] == "ok":
            per_draw[int(r["query"]), int(r["rep"])].append(float(r["base_us"]) / float(r["accel_us"]))
    return {q: statistics.median(geomean(v) for (qq, _), v in per_draw.items() if qq == q) for q in QUERIES}


def fmt(v):
    """Two significant figures; thousands as k (4.5k, 40k)."""
    if v >= 1000:
        k = v / 1000
        return f"{k:.1f}k" if k < 10 else f"{k:.0f}k"
    return f"{v:.1f}" if v < 10 else f"{v:.0f}"


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else HERE
    os.makedirs(out_dir, exist_ok=True)
    gendb = {r["query"]: {k: float(v) for k, v in r.items() if k != "query"}
             for r in csv.DictReader(open(os.path.join(DATA, "gendb_times.csv")))}
    pg, pg_verified = postgres_speedup("cpp"), postgres_speedup("verified")
    panels = [  # title, [(label, color, speedup per query)]
        ("DuckDB", [("JetStream", BLUE, [duckdb_speedup(q, "cpp", "cpp") for q in QUERIES]),
                    ("JetStream Verified", PURPLE, [duckdb_speedup(q, "verified", "verus") for q in QUERIES]),
                    ("GenDB", ORANGE, [gendb[f"Q{q}"]["duckdb_host_ms"] / gendb[f"Q{q}"]["gendb_ms"] for q in QUERIES])]),
        ("Postgres", [("JetStream", BLUE, [pg[q] for q in QUERIES]),
                      ("JetStream Verified", PURPLE, [pg_verified[q] for q in QUERIES]),
                      ("GenDB", ORANGE, [gendb[f"Q{q}"]["postgres_host_ms"] / gendb[f"Q{q}"]["gendb_ms"] for q in QUERIES])]),
    ]
    for title, series in panels:
        for label, _, vals in series:
            print(f"{title:9s} {label:19s} " + " ".join(f"Q{q}={fmt(v):>5s}" for q, v in zip(QUERIES, vals))
                  + f"  geomean={fmt(geomean(vals))}")
    # Single-column figure: 3.33 in wide, fonts at the paper's ~7 pt.
    plt.rcParams.update({"font.size": 7, "pdf.fonttype": 42, "ps.fonttype": 42, "axes.linewidth": 0.6, "xtick.major.width": 0.6,
                         "ytick.major.width": 0.6, "xtick.major.size": 2, "ytick.major.size": 2, "xtick.major.pad": 1.5,
                         "ytick.major.pad": 1.5})
    fig, axes = plt.subplots(1, 2, figsize=(3.33, 1.6), sharey=True)
    x = np.arange(len(QUERIES))
    width = 0.27
    peak = max(max(vals) for _, series in panels for *_, vals in series)
    for ax, (title, series) in zip(axes, panels):
        for i, (label, color, vals) in enumerate(series):
            pos = x + (i - (len(series) - 1) / 2) * width
            ax.bar(pos, vals, width * 0.92, color=color, linewidth=0, label=label, zorder=2)
            for p, v in zip(pos, vals):
                ax.text(p, v * 1.25, fmt(v), rotation=90, rotation_mode="anchor", ha="left", va="center_baseline",
                        fontsize=5.5, color="#222222")
        ax.set_yscale("log")
        ax.axhline(1, color="#666666", linestyle="--", linewidth=0.5, zorder=1)
        ax.set_xticks(x, [f"Q{q}" for q in QUERIES])
        ax.set_xlim(x[0] - 0.55, x[-1] + 0.55)
        ax.set_title(title, fontsize=7, pad=2)
        ax.tick_params(axis="x", length=0)
    ax = axes[0]
    ax.set_ylim(0.5, peak * 25)
    ax.yaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, numticks=12))
    ax.yaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.set_ylabel("Read speedup (×)", labelpad=1)
    # Title, then the legend, above both panels.
    fig.tight_layout(pad=0.2, w_pad=0.4, rect=(0, 0, 1, 0.83))
    fig.suptitle("SEC-EDGAR (SF=3): Read Speedup over Host Engine", fontsize=7.5, y=1.0, va="top")
    fig.legend(*ax.get_legend_handles_labels(), ncol=3, frameon=False, loc="upper center", bbox_to_anchor=(0.5, 0.925),
               borderaxespad=0, handlelength=1.2, handletextpad=0.4, columnspacing=1.2)
    out = os.path.join(out_dir, "sec_edgar_read_speedup_vs_gendb")
    for ext in ("pdf", "png"):
        fig.savefig(f"{out}.{ext}", dpi=1200, bbox_inches="tight", pad_inches=0.02)
    print(f"wrote {out}.pdf / .png")


if __name__ == "__main__":
    main()
