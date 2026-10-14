#!/usr/bin/env python3
"""Per-query workload speedup over DuckDB for each write interval, from a run_rf_interval_sweep.sh directory.

  python3 eval/compare/plot_rf_interval_sweep.py <out_dir> [--system cpp] [--png path]

Reads <out_dir>/rf_interval_summary.csv (summarize_rf_interval_sweep.py) and plots workload_speedup_vs_duckdb, median over
repetitions, one bar per interval, each interval labelled by the rows/s its refresh stream touches; the geomean group
is annotated with its values.  Writes <out_dir>/rf_interval_workload.{png,pdf} (paper figure).
"""
import argparse
import csv
import math
import os
import statistics


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--system", default="cpp")
    ap.add_argument("--title", default=None)
    ap.add_argument("--png", default=None)
    a = ap.parse_args()

    by, rows_s = {}, {}
    for r in csv.DictReader(open(os.path.join(a.out, "rf_interval_summary.csv"))):
        T = float(r["interval_s"])
        if r["system"] == "base":
            rows_s.setdefault(T, []).append(float(r["rows_touched_per_s"]))
        elif r["system"] == a.system and r["workload_speedup_vs_duckdb"]:
            by.setdefault((T, int(r["query"])), []).append(r)
    intervals = sorted({T for T, _ in by}, reverse=True)  # slowest writes first
    qs = sorted({q for _, q in by})

    def med(T, q):
        rs = by.get((T, q), [])
        if not rs:
            return None, True
        return (statistics.median(float(r["workload_speedup_vs_duckdb"]) for r in rs),
                all(r["keeps_up"] == "True" for r in rs))

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter
    plt.rcParams.update({"font.size": 8, "pdf.fonttype": 42, "ps.fonttype": 42})
    # one-hue ordinal ramp, light -> dark as the write load grows
    colors = ["#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#0d366b"]
    labels = qs + ["GM"]
    x = np.arange(len(labels), dtype=float)
    gw = 2.4  # the geomean group is wider so its values fit above the bars
    x[-1] += 0.6 + gw / 2
    width = 0.84 / len(intervals)
    fig, ax = plt.subplots(figsize=(7, 2.4))
    ys_all = []
    for j, T in enumerate(intervals):
        ys = [med(T, q)[0] for q in qs]
        logs = [math.log(y) for y in ys if y]
        gm = math.exp(sum(logs) / len(logs))
        ys_all += [y for y in ys if y] + [gm]
        rs = statistics.median(rows_s.get(T, [0]))
        off = j - (len(intervals) - 1) / 2
        c = colors[j % len(colors)]
        ax.bar(x[:-1] + off * width, [y or 0 for y in ys], width * 0.9, color=c, linewidth=0, label=f"{rs:,.0f} rows/s")
        gx = x[-1] + off * width * gw
        ax.bar(gx, gm, width * gw * 0.9, color=c, linewidth=0)
        ax.text(gx, gm * 1.1, f"{gm:,.0f}", ha="center", va="bottom", rotation=90, fontsize=7)
    ax.set_yscale("log")
    ax.set_ylim(max(1, min(ys_all) * 0.5), max(ys_all) * 2.2)
    ax.yaxis.set_major_locator(LogLocator(base=10))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.set_xticks(x, [f"Q{q}" if q != "GM" else "GM" for q in labels])
    ax.tick_params(axis="x", length=0, labelsize=7)
    ax.set_xlim(x[0] - 0.6, x[-1] + gw / 2)
    ax.set_ylabel("Speedup over DuckDB")
    ax.set_title(a.title or "TPC-H (SF=20): Workload Speedup across Write Rates", fontsize=9, pad=22)
    ax.grid(axis="y", which="major", color="#dddddd", lw=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.legend(frameon=False, loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=len(intervals),
              handlelength=1.2, columnspacing=1.5)
    fig.tight_layout(pad=0.2)
    png = a.png or os.path.join(a.out, "rf_interval_workload.png")
    fig.savefig(png, dpi=300)
    pdf = os.path.splitext(png)[0] + ".pdf"
    fig.savefig(pdf)
    print("wrote", png, "and", pdf)

if __name__ == "__main__":
    main()
