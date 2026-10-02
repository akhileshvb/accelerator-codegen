#!/usr/bin/env python3
"""Q1 workload speedup over plain DuckDB, by write phase and maintenance strategy.

Input: the CSV from `maintenance_choice_demo <db> 1 phases <base db> <out.csv>`
(per phase and strategy: mean write ms incl. maintenance, mean read ms).
For R reads per write, a phase's cost per write is write_ms + R * read_ms;
speedup = plain DuckDB's cost / the strategy's cost.

Several CSVs (independent runs): each measurement is their median.

Usage: plot_q1_maintenance_phases.py <q1_phases.csv>... <out.pdf>
"""
import csv
import statistics
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.backends.backend_pdf import PdfPages  # noqa: E402

# Validated categorical palette (slots 1-4, light surface #fcfcfb).
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
STRATEGIES = [
    # (csv name, label, color, marker, line width)
    ("selected", "Cost model (selected)", "#2a78d6", "o", 2.6),
    ("ApplyDelta", "Always ApplyDelta", "#eb6834", "s", 1.6),
    ("RebuildPartition", "Always RebuildPartition", "#1baf7a", "^", 1.6),
    ("RebuildAll", "Always RebuildAll", "#eda100", "D", 1.6),
]
READS_PER_WRITE = [1, 10, 100]
SHORT = {
    "ApplyDelta": "Delta",
    "RebuildPartition": "Partition",
    "RebuildAll": "All",
}


def load(paths):
    phases = []
    runs = defaultdict(lambda: defaultdict(list))
    for path in paths:
        for r in csv.DictReader(open(path)):
            if r["phase"] not in phases:
                phases.append(r["phase"])
            runs[r["strategy"]][r["phase"]].append(r)
    data = defaultdict(dict)
    for strategy, by_phase in runs.items():
        for phase, rs in by_phase.items():
            merged = dict(rs[0])
            for key in ("write_ms", "read_ms"):
                merged[key] = str(statistics.median(float(r[key]) for r in rs))
            data[strategy][phase] = merged
    return phases, data


def cost(r, reads):
    return float(r["write_ms"]) + reads * float(r["read_ms"])


def main():
    phases, data = load(sys.argv[1:-1])
    out = sys.argv[-1]
    labels = [
        f"{p}\n{int(data['base'][p]['rows']):,} rows · {int(data['base'][p]['days']):,} days"
        for p in phases
    ]
    plt.rcParams.update({
        "font.size": 9,
        "axes.edgecolor": INK_2,
        "axes.labelcolor": INK,
        "xtick.color": INK_2,
        "ytick.color": INK_2,
        "text.color": INK,
    })
    with PdfPages(out) as pdf:
        fig, axes = plt.subplots(1, len(READS_PER_WRITE), figsize=(15, 5.4), sharey=True,
                                 facecolor=SURFACE)
        for ax, reads in zip(axes, READS_PER_WRITE):
            ax.set_facecolor(SURFACE)
            xs = range(len(phases))
            for name, label, color, marker, lw in STRATEGIES:
                ys = [cost(data["base"][p], reads) / cost(data[name][p], reads) for p in phases]
                ax.plot(xs, ys, color=color, marker=marker, markersize=7, linewidth=lw,
                        label=label, zorder=3 if name == "selected" else 2,
                        markeredgecolor=SURFACE, markeredgewidth=1.5)
                if name == "selected":
                    # The action the cost model chose in each phase.
                    for x, p, y in zip(xs, phases, ys):
                        ax.annotate(SHORT.get(data[name][p]["chosen"], data[name][p]["chosen"]),
                                    (x, y), textcoords="offset points", xytext=(0, 9),
                                    ha="center", fontsize=7.5, color=INK_2)
            ax.axhline(1.0, color=INK, linewidth=1.2, zorder=1)
            ax.text(len(phases) - 0.5, 1.0, "plain DuckDB (1×)", color=INK_2, fontsize=7.5,
                    va="bottom", ha="right")
            ax.set_yscale("log")
            ax.set_xticks(list(xs))
            ax.set_xticklabels(labels, fontsize=7, rotation=30, ha="right")
            ax.set_title(f"{reads} read{'s' if reads > 1 else ''} per write", fontsize=10)
            ax.grid(axis="y", color=GRID, linewidth=0.8, which="both")
            for side in ("top", "right"):
                ax.spines[side].set_visible(False)
        axes[0].set_ylabel("Workload speedup over plain DuckDB (log scale)")
        handles, legend_labels = axes[0].get_legend_handles_labels()
        fig.legend(handles, legend_labels, loc="upper center", ncol=4, frameon=False, fontsize=9,
                   bbox_to_anchor=(0.5, 0.94))
        fig.suptitle("TPC-H Q1 (SF1, DuckDB): speedup by write phase and maintenance strategy",
                     fontsize=12, color=INK)
        fig.text(0.5, 0.005,
                 "Per phase: 4 writes (UPDATE l_quantity on the phase's rows), each followed by reads of Q1. "
                 "Cost per write = write time (incl. capture and maintenance) + reads × read time. "
                 "Labels on the cost-model line: the action it chose.",
                 ha="center", fontsize=7.5, color=INK_2)
        fig.tight_layout(rect=(0, 0.03, 1, 0.9))
        pdf.savefig(fig, facecolor=SURFACE)
        plt.close(fig)

        # Table view of the measurements.
        fig, ax = plt.subplots(figsize=(15, 5.4), facecolor=SURFACE)
        ax.axis("off")
        header = ["Phase", "Rows", "Days", "Plain DuckDB\nwrite / read ms"] + [
            f"{label}\nwrite / read ms" for _, label, *_ in STRATEGIES] + ["Chosen action"]
        cells = []
        for p in phases:
            b = data["base"][p]
            row = [p, f"{int(b['rows']):,}", f"{int(b['days']):,}",
                   f"{float(b['write_ms']):.1f} / {float(b['read_ms']):.1f}"]
            for name, *_ in STRATEGIES:
                r = data[name][p]
                row.append(f"{float(r['write_ms']):.1f} / {float(r['read_ms']):.2f}")
            row.append(data["selected"][p]["chosen"])
            cells.append(row)
        table = ax.table(cellText=cells, colLabels=header, loc="center", cellLoc="center")
        table.auto_set_font_size(False)
        table.set_fontsize(8)
        table.scale(1, 2.2)
        for (r, _), cell in table.get_celld().items():
            cell.set_edgecolor(GRID)
            if r == 0:
                cell.set_text_props(color=INK, weight="bold")
        ax.set_title("Measurements (mean per write and per read, SF1; median of the runs)", color=INK)
        pdf.savefig(fig, facecolor=SURFACE)
        plt.close(fig)


if __name__ == "__main__":
    main()
