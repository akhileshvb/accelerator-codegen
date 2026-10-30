#!/usr/bin/env python3
"""Figure 10, agent pipeline ablation: geomean read speedup over DuckDB vs mean synthesis cost per query, over TPC-H
Q1, Q4, Q6, Q7, Q11, Q12 and Q16. One point per generator (JetStream with learned memory and proof library: warm;
without: cold; a single Claude Code agent with the same substrate, tests and acceptance criteria: raw) and kind
(tested: cpp, verified: verus). A point is hollow when its accelerators were flagged or failed the hidden tests.

Usage: plot_fig10.py [points.csv] [OUT_PREFIX]
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator

HERE = Path(__file__).resolve().parent

ARMS = {  # arm: (label, color)
    "raw": ("Claude Code w/ JetStream substrate", "#2a78d6"),
    "cold": ("JetStream (no memory)", "#eb6834"),
    "warm": ("JetStream (with memory)", "#1baf7a"),
}
KINDS = {"cpp": ("Tested", "o"), "verus": ("Verified", "^")}  # kind: (label, marker)

WIDTH_IN = 3.4
HEIGHT_IN = 2.1
LEGEND_SIZE = 6.5
TICK_SIZE = 7
AXIS_LABEL_SIZE = 7.5
plt.rcParams.update({"pdf.fonttype": 42, "ps.fonttype": 42})  # embed TrueType fonts, as paper templates require


def load(path: Path) -> list[dict]:
    with open(path) as f:
        return [{"kind": r["kind"], "arm": r["arm"], "cost": float(r["mean_cost_usd"]),
                 "speedup": float(r["geomean_speedup"]), "correct": r["correct"] == "true"} for r in csv.DictReader(f)]


def main() -> None:
    points_csv = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "data/points.csv"
    out_prefix = Path(sys.argv[2]) if len(sys.argv) > 2 else HERE / "data/speedup_vs_cost"
    pts = load(points_csv)

    fig, ax = plt.subplots(figsize=(WIDTH_IN, HEIGHT_IN), facecolor="white", layout="constrained")
    fig.get_layout_engine().set(w_pad=0.02, h_pad=0.02, rect=(0, 0, 0.98, 1))
    for p in pts:
        color = ARMS[p["arm"]][1]
        filled = p["correct"]
        ax.scatter(p["cost"], p["speedup"], s=70, marker=KINDS[p["kind"]][1], zorder=4 if p["arm"] == "raw" else 3,
                   facecolors=color if filled else "white", edgecolors="white" if filled else color,
                   linewidths=0.6 if filled else 1.2)
        print(f"{p['kind']:>5} {p['arm']:>4}  mean cost ${p['cost']:.2f}  geomean speedup {p['speedup']:.1f}x"
              f"  {'correct' if filled else 'hollow'}")

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.xaxis.set_major_locator(FixedLocator([1, 3, 10, 30]))
    ax.yaxis.set_major_locator(FixedLocator([10, 30, 100, 300]))
    for axis in (ax.xaxis, ax.yaxis):
        axis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"${v:g}"))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    ax.set_xlim(1, 60)
    ax.set_ylim(8, 400)
    ax.set_xlabel("Mean cost per query (USD) (lower is better)", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("Speedup over DuckDB (×)\n(higher is better)", fontsize=AXIS_LABEL_SIZE)
    ax.tick_params(labelsize=TICK_SIZE)
    ax.grid(True, which="major", color="#e6e6e6", lw=0.5, zorder=0)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

    handles = [Patch(facecolor=c, edgecolor="none", label=l) for l, c in ARMS.values()]
    handles += [plt.Line2D([], [], ls="", marker=m, mfc="#555555", mec="#555555", markersize=5.5, label=l)
                for l, m in KINDS.values()]
    if not all(p["correct"] for p in pts):
        handles.append(plt.Line2D([], [], ls="", marker="o", mfc="white", mec="#555555", mew=1.0, markersize=5.5,
                                  label="Flagged / incorrect"))
    fig.legend(handles=handles, loc="outside upper center", ncol=2, frameon=False, fontsize=LEGEND_SIZE,
               handlelength=1.0, handleheight=0.8, handletextpad=0.4, columnspacing=1.2, labelspacing=0.25)

    out_prefix.parent.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(out_prefix.with_suffix(f".{ext}"), dpi=600, facecolor="white")
        print(f"wrote {out_prefix.with_suffix('.' + ext)}")


if __name__ == "__main__":
    main()
