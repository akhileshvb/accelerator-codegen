#!/usr/bin/env python3
"""Cumulative workload time through write phases (cost model vs fixed actions).

Input: the CSV from `maintenance_choice_demo <db> 1 trace <base db> <out.csv>`
(per query index and strategy: ms for the write before it, if any, with its
maintenance, plus the query; TRACE_ROUNDS rounds).

Top: cumulative ms (per-query median over rounds, summed) per strategy, with the
phase bands and, below the curves, the action the cost model chose at each write.
Bottom: per phase, the workload speedup over plain DuckDB (its total ms over the
strategy's total ms, writes included).

Usage: plot_q1_maintenance_cumulative.py <trace.csv> <out.pdf> [title]
"""
import csv
import sys
from collections import Counter, defaultdict
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

PHASE_COLORS = {"A": "#a9c6e3", "B": "#f8cfa6", "C": "#b9dfb2", "D": "#f2b7b7"}
PHASE_NAMES = {
    "A": "A: point write\nbefore every query",
    "B": "B: bulk write\nevery 40 queries",
    "C": "C: mixed point\nand bulk writes",
    "D": "D: read-heavy,\nrare point writes",
}
LINES = [
    ("base", "Plain DuckDB", "#8c8c8c", 1.4),
    ("RebuildAll", "Always full rebuild", "#d62728", 1.4),
    ("ApplyDelta", "Always incremental", "#7b2cbf", 1.4),
    ("selected", "Cost model", "#000000", 2.0),
]
# small-batch: the module applied the deltas without consulting the cost model.
ACTION_COLORS = {"ApplyDelta": "#7b2cbf", "small-batch": "#7b2cbf", "RebuildAll": "#d62728",
                 "RebuildPartition": "#d62728"}


def main():
    path, out = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else "TPC-H Q1 (SF=20) under changing write load"
    samples = defaultdict(lambda: defaultdict(list))
    actions = defaultdict(Counter)  # cost model's action per write index, over rounds
    phase, write = {}, {}
    for r in csv.DictReader(open(path)):
        i = int(r["index"])
        samples[r["strategy"]][i].append(float(r["ms"]))
        phase[i] = r["phase"]
        write[i] = r["write"]
        if r["strategy"] == "selected" and r["action"]:
            actions[i][r["action"]] += 1
    ms = {s: {i: median(v) for i, v in per.items()} for s, per in samples.items()}
    xs = sorted(phase)
    phases = sorted(set(phase.values()))

    plt.rcParams.update({"font.size": 12})
    fig, (ax, bars) = plt.subplots(2, 1, figsize=(10, 7.2), gridspec_kw={"height_ratios": [2.2, 1]})

    # Phase bands.
    spans = []
    start = xs[0]
    for i in xs[1:] + [None]:
        if i is None or phase[i] != phase[start]:
            end = i if i is not None else xs[-1] + 1
            spans.append((phase[start], start, end))
            ax.axvspan(start, end, color=PHASE_COLORS[phase[start]], alpha=0.35, linewidth=0, zorder=0)
            start = i

    top, ends = 0.0, []
    for name, label, color, lw in LINES:
        acc, ys = 0.0, []
        for i in xs:
            acc += ms[name][i]
            ys.append(acc / 1000)
        top = max(top, ys[-1])
        ax.plot(xs, ys, color=color, linewidth=lw, zorder=3 if name == "selected" else 2)
        ends.append(ys[-1])
    # End labels, spread apart so close finishes don't overlap.
    placed = []
    for y in sorted(ends):
        label_y = max(y, placed[-1] + top * 0.045) if placed else y
        placed.append(label_y)
        ax.annotate(f"{y:.0f} s", (xs[-1], y), xytext=(xs[-1] + 4, label_y), textcoords="data",
                    va="center", fontsize=10, color="#333333", annotation_clip=False)
    # The cost model's action at each write (most common over rounds).
    strip_y = -top * 0.05
    for i in xs:
        if write[i] and actions[i]:
            a = actions[i].most_common(1)[0][0]
            # A point write committed while a bulk write's rebuild still runs
            # is folded into that rebuild; its log still shows the bulk's
            # decision. Gray, not a rebuild chosen for the point write.
            folded = write[i] == "point" and a.startswith("Rebuild")
            ax.plot([i, i], [strip_y * 0.4, strip_y * 1.6],
                    color="#9e9e9e" if folded else ACTION_COLORS.get(a, "#444444"),
                    linewidth=1.6 if write[i] == "bulk" else 0.6, solid_capstyle="butt")
    ax.set_ylim(strip_y * 2.2, top * 1.05)
    ax.set_xlim(xs[0], xs[-1] + 1)
    ax.set_ylabel("Cumulative time (s)")
    ax.set_xlabel("Query index")
    ax.set_title(title)
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="#e6e6e6", linewidth=0.6)

    handles = [Line2D([], [], color=c, linewidth=2, label=l) for _, l, c, _ in reversed(LINES)]
    handles += [Line2D([], [], color="#444444", linewidth=0,
                       label="Ticks below 0: cost model's action per write\n(purple incremental, red rebuild,\ngray: folded into a running rebuild)")]
    ax.legend(handles=handles, loc="upper left", frameon=False, fontsize=10)

    # Per-phase workload speedup over plain DuckDB.
    strategies = [s for s in LINES if s[0] != "base"]
    width = 0.8 / len(strategies)
    for k, (name, label, color, _) in enumerate(strategies):
        vals = []
        for p in phases:
            idx = [i for i in xs if phase[i] == p]
            vals.append(sum(ms["base"][i] for i in idx) / sum(ms[name][i] for i in idx))
        pos = [j + (k - (len(strategies) - 1) / 2) * width for j in range(len(phases))]
        bars.bar(pos, vals, width * 0.92, color=color, label=label)
        for x, v in zip(pos, vals):
            bars.annotate(f"{v:.1f}×" if v < 10 else f"{v:.0f}×", (x, v), xytext=(0, 2),
                          textcoords="offset points", ha="center", fontsize=8.5, color="#333333")
    bars.axhline(1.0, color="#8c8c8c", linewidth=0.8, linestyle="--")
    bars.set_yscale("log")
    bars.set_xticks(range(len(phases)), [PHASE_NAMES.get(p, p) for p in phases], fontsize=10)
    bars.set_ylabel("Speedup vs DuckDB\n(per phase, log)")
    bars.spines[["top", "right"]].set_visible(False)
    bars.legend(loc="upper center", ncol=3, frameon=False, fontsize=10, bbox_to_anchor=(0.5, 1.18))

    fig.tight_layout()
    fig.savefig(out)


if __name__ == "__main__":
    main()
