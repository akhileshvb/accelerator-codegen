#!/usr/bin/env python3
"""Plot the read-only experiment: per-query speedup over DuckDB, C++ vs Verus.

  python3 eval/compare/plot_read.py <read_dir>

<read_dir>/read_q<N>_<cpp|verus>_r<k>.json (run_compare.py read; one file per repetition, each
with its own parameter draw).  speedup_geomean in a file is the geomean over that draw's
parameter sets.  Bars are the median over the repetitions, whiskers their min..max; the last group is
the geomean over the queries (per repetition, then median/min..max).  Writes <read_dir>/read_speedup.png.
"""
import glob
import json
import os
import re
import sys

import matplotlib.pyplot as plt

from plotlib import LARGE_FONTS, geomean, stat

plt.rcParams.update(LARGE_FONTS)

SYSTEMS = [("cpp", "C++", "#1e40af"), ("verus", "Verus", "#166534")]


def load(d):
    runs = {}  # system -> query -> {rep: speedup}
    for f in glob.glob(os.path.join(d, "read_q*_r*.json")):
        m = re.match(r"read_q(\d+)_(\w+?)_r(\d+)\.json", os.path.basename(f))
        r = json.load(open(f))
        if not m or "error" in r or not r.get("speedup_geomean"):
            continue
        runs.setdefault(m.group(2), {}).setdefault(int(m.group(1)), {})[int(m.group(3))] = r["speedup_geomean"]
    return runs


def main():
    d = sys.argv[1]
    runs = load(d)
    if not runs:
        sys.exit(f"no read_q*_r*.json under {d}")
    queries = sorted({q for s in runs.values() for q in s})
    labels = [f"Q{q}" for q in queries] + ["Geomean"]
    fig, ax = plt.subplots(figsize=(24, 7.5))
    w = 0.38
    for i, (s, label, color) in enumerate(SYSTEMS):
        if s not in runs:
            continue
        st = [stat(runs[s][q].values()) if q in runs[s] else None for q in queries]
        reps = sorted(set.intersection(*[set(runs[s][q]) for q in queries if q in runs[s]]))
        st.append(stat([geomean([runs[s][q][k] for q in queries]) for k in reps]))
        xs = [j + (i - 0.5) * w for j in range(len(st))]
        pts = [(x, t) for x, t in zip(xs, st) if t]
        ax.bar([x for x, _ in pts], [t[0] for _, t in pts], w, color=color, label=label, edgecolor="white", linewidth=1.5,
               yerr=[[t[0] - t[1] for _, t in pts], [t[2] - t[0] for _, t in pts]],
               error_kw={"ecolor": "#374151", "elinewidth": 1.3, "capsize": 3})
        for x, t in pts:  # the median, above the whisker
            ax.annotate(("%.1fx" if t[0] < 10 else "%.0fx") % t[0], (x, t[2]), textcoords="offset points",
                        xytext=(0, 4), ha="center", va="bottom", rotation=90, fontsize=12)
    ax.axhline(1, color="#6b7280", lw=1.5, ls="--", label="DuckDB (1x)")
    ax.axvline(len(queries) - 0.5, color="#d1d5db", lw=1)
    ax.set_yscale("log")
    ax.margins(y=0.2)
    ax.set_xticks(range(len(labels)), labels)
    ax.set_xlim(-0.6, len(labels) - 0.1)
    ax.set_ylabel("Read speedup over DuckDB (log scale)")
    ax.set_title("Read-only speedup over DuckDB per TPC-H query (median reported)\n↑ Higher is better")
    ax.grid(axis="y", alpha=0.3)
    ax.set_axisbelow(True)
    ax.legend(loc="upper left")
    fig.tight_layout()
    png = os.path.join(d, "read_speedup.png")
    fig.savefig(png, dpi=120)
    print("wrote " + png)


if __name__ == "__main__":
    main()
