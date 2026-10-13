#!/usr/bin/env python3
"""Per-query speedup over plain DuckDB through write phases (Q1).

Input: the CSV from `maintenance_choice_demo <db> 1 trace <base db> <out.csv>`
(per query index and strategy: ms for the write before it, if any, with its
maintenance, plus the query). Speedup of query i = plain DuckDB's ms / the
strategy's ms.

Usage: plot_q1_maintenance_trace.py [--read] <q1_trace.csv> <out.pdf> [title]

--read: the query alone (read_ms column), without the write before it.
"""
import csv
import sys
from collections import defaultdict
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

PHASE_COLORS = {"A": "#a9c6e3", "B": "#f8cfa6", "C": "#b9dfb2", "D": "#f2b7b7"}
LINES = [
    ("selected", "Cost model", "#000000", 1.6),
    ("ApplyDelta", "Always incremental", "#7b2cbf", 1.1),
    ("RebuildAll", "Always full rebuild", "#d62728", 1.1),
]
POINT_COLOR = "#1f77b4"
BULK_COLOR = "#2ca02c"


def main():
    args = sys.argv[1:]
    read_only = "--read" in args
    args = [a for a in args if a != "--read"]
    path, out = args[0], args[1]
    column = "read_ms" if read_only else "ms"
    title = args[2] if len(args) > 2 else "Cost Model: TPC-H (SF=1), Q1 under Write Load"
    # Per query and strategy: the median over rounds (TRACE_ROUNDS).
    samples = defaultdict(lambda: defaultdict(list))
    phase, write = {}, {}
    for r in csv.DictReader(open(path)):
        i = int(r["index"])
        samples[r["strategy"]][i].append(float(r[column]))
        phase[i] = r["phase"]
        write[i] = r["write"]
    ms = {s: {i: median(v) for i, v in per.items()} for s, per in samples.items()}
    xs = sorted(phase)

    plt.rcParams.update({"font.size": 13})
    fig, ax = plt.subplots(figsize=(10, 4.6))
    speed = ax.twinx()

    # Everything on the speedup axis; the left axis only carries the
    # "Writes" label (the markers sit at a fixed height).
    start = xs[0]
    for i in xs[1:] + [None]:
        if i is None or phase[i] != phase[start]:
            end = (i if i is not None else xs[-1] + 1)
            speed.axvspan(start, end, color=PHASE_COLORS[phase[start]], alpha=0.45, linewidth=0,
                          zorder=0)
            start = i
    top = 0.0
    for name, _, color, lw in LINES:
        ys = [ms["base"][i] / ms[name][i] for i in xs]
        top = max(top, max(ys))
        speed.plot(xs, ys, color=color, linewidth=lw, zorder=3 if name == "selected" else 2)
    speed.set_ylim(0, top * 1.12)
    speed.set_ylabel("Read speedup vs DuckDB" if read_only else "Speedup vs DuckDB")
    marker_y = top * 0.14
    point_x = [i for i in xs if write[i] == "point"]
    bulk_x = [i for i in xs if write[i] == "bulk"]
    speed.scatter(point_x, [marker_y] * len(point_x), s=22, color=POINT_COLOR, zorder=4)
    speed.scatter(bulk_x, [marker_y] * len(bulk_x), s=22, color=BULK_COLOR, zorder=4)
    ax.set_yticks([])
    ax.set_ylabel("Writes")
    ax.set_xlim(xs[0], xs[-1] + 1)
    ax.set_xlabel("Global Query Index")
    ax.set_title(title)

    handles = [Patch(color=PHASE_COLORS[p], alpha=0.6, label=f"Phase {p}") for p in "ABCD"]
    handles += [
        Line2D([], [], marker="o", linestyle="", color=POINT_COLOR, label="Point Writes"),
        Line2D([], [], marker="o", linestyle="", color=BULK_COLOR, label="Bulk Writes"),
    ]
    handles += [Line2D([], [], color=color, linewidth=2, label=label) for _, label, color, _ in LINES]
    order = [0, 1, 2, 3, 4, 5, 6, 7, 8]
    fig.legend([handles[i] for i in order], [handles[i].get_label() for i in order],
               loc="lower center", ncol=3, frameon=False, fontsize=12, bbox_to_anchor=(0.5, 0.0),
               handlelength=1.6, columnspacing=1.5)
    fig.tight_layout(rect=(0, 0.2, 1, 1))
    fig.savefig(out)


if __name__ == "__main__":
    main()
