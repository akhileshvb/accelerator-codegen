#!/usr/bin/env python3
"""Rolling speedup over DuckDB through write phases (cost model vs fixed actions).

Input: the CSV from `maintenance_choice_demo <db> 1 trace <base db> <out.csv>`
(per query index and strategy: ms for the write before it, if any, with its
maintenance, plus the query; TRACE_ROUNDS rounds).

Per strategy, DuckDB's total ms over the strategy's total ms in the last W query
indices (per-index median over rounds; writes and maintenance included), with
the phase bands and the writes marked along the bottom.

Also prints, per phase and overall, DuckDB's total ms over each strategy's total ms.

Usage: plot_fig9.py [trace.csv] [OUT_DIR] [window]   (writes OUT_DIR/q1_maintenance.{pdf,png})
"""
import csv
import sys
from pathlib import Path
from collections import defaultdict
from statistics import median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

PHASE_COLORS = {"A": "#a9c6e3", "B": "#f8cfa6", "C": "#b9dfb2", "D": "#f2b7b7"}
LINES = [
    ("RebuildAll", "Always full rebuild", "#d62728", 0.9),
    ("ApplyDelta", "Always incremental", "#7b2cbf", 0.9),
    ("selected", "Cost model", "#000000", 1.3),
]

HERE = Path(__file__).resolve().parent
csv_path = sys.argv[1] if len(sys.argv) > 1 else HERE / "data/q1_trace_contrast_sf20.csv"
out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "data"
W = int(sys.argv[3]) if len(sys.argv) > 3 else 20

runs = defaultdict(lambda: defaultdict(list))
phase, write = {}, {}
for r in csv.DictReader(open(csv_path)):
    i = int(r["index"])
    runs[r["strategy"]][i].append(float(r["ms"]))
    phase[i], write[i] = r["phase"], r["write"]
ms = {s: {i: median(v) for i, v in d.items()} for s, d in runs.items()}
xs = sorted(phase)

for p in sorted(set(phase.values())) + ["all"]:
    idx = [i for i in xs if p in ("all", phase[i])]
    base = sum(ms["base"][i] for i in idx)
    print(f"{p:>3}  " + "  ".join(f"{label} {base / sum(ms[s][i] for i in idx):.2f}x" for s, label, _, _ in LINES))

# Drawn at single-column size (3.4 in) so fonts print at their nominal size; kept short
# (no title, legend on top) to save vertical space in the paper.
plt.rcParams.update({"font.size": 8, "axes.labelsize": 8, "xtick.labelsize": 7, "ytick.labelsize": 7,
                     "axes.linewidth": 0.6, "pdf.fonttype": 42, "ps.fonttype": 42})
fig, ax = plt.subplots(figsize=(3.4, 1.7))

start = xs[0]
for i in xs[1:] + [None]:
    if i is None or phase[i] != phase[start]:
        end = i if i is not None else xs[-1] + 1
        ax.axvspan(start, end, color=PHASE_COLORS[phase[start]], alpha=0.35, lw=0, zorder=0)
        ax.text((start + end) / 2, 0.98, phase[start], transform=ax.get_xaxis_transform(),
                ha="center", va="top", fontsize=8, fontweight="bold", color="#444444")
        start = i

for s, label, color, lw in LINES:
    ys = []
    for j in xs:
        win = [i for i in xs if j - W < i <= j]
        ys.append(sum(ms["base"][i] for i in win) / sum(ms[s][i] for i in win))
    ax.plot(xs, ys, color=color, lw=lw, zorder=3 if s == "selected" else 2)

ax.axhline(1, color="#8c8c8c", lw=0.6, ls="--")
ax.text(xs[-1], 1.08, "DuckDB", ha="right", va="bottom", fontsize=6.5, color="#666666")
ax.set_yscale("log")
ax.set_xlim(xs[0], xs[-1] + 1)
ax.set_ylim(None, ax.get_ylim()[1] * 3)  # headroom for the phase letters
lo = ax.get_ylim()[0]
for i in xs:
    if write[i]:
        bulk = write[i] == "bulk"
        ax.plot([i, i], [lo, lo * (1.6 if bulk else 1.25)],
                color="#2ca02c" if bulk else "#1f77b4", lw=1.0 if bulk else 0.3)
ax.set_ylim(lo, None)
ax.set_ylabel("Speedup vs DuckDB", labelpad=1)
ax.set_xlabel("Query index", labelpad=1)
ax.spines[["top", "right"]].set_visible(False)

handles = [Line2D([], [], color=c, lw=1.5, label=l) for _, l, c, _ in reversed(LINES)]
handles += [Line2D([], [], color="#1f77b4", lw=0.8, label="Point write"),
            Line2D([], [], color="#2ca02c", lw=1.5, label="Bulk write")]
fig.legend(handles=handles, loc="upper center", ncol=3, frameon=False, fontsize=6.5,
           handlelength=1.2, columnspacing=0.9, handletextpad=0.3, borderaxespad=0.05, labelspacing=0.15)
fig.tight_layout(rect=(0, 0, 1, 0.85), pad=0.15)
out_dir.mkdir(parents=True, exist_ok=True)
fig.savefig(out_dir / "q1_maintenance.pdf")
fig.savefig(out_dir / "q1_maintenance.png", dpi=600)
