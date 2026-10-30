#!/usr/bin/env python3
"""Figure 8: geomean workload speedup across write rates, plus PG memory footprint (one-column, three panels).

(A) JetStream (stateful) vs DuckDB: per write interval, the GM of workload_speedup_vs_duckdb (system cpp) from
    data/writerate/rf_interval_summary.csv, labelled by the base run's rows/s; read-only is the Figure 5 geomean.
    Run: cores 0-39, every thread knob 40, 8 readers, one writer on the official RF1/RF2 sets + dimension writes,
    1 repetition, 8 parameter sets, tree at 29ed49f7 (scripts/drive_writerate.sh).
    Maintenance fields were corrected after the run (aux_compare at 29ed49f7 logged every maintenance action as
    "none"; fixed on fix/conc-decision-log). Timings, write streams and answers are unaffected (rerun of T=5s,
    same seed: same writes, reader QPS ratio median 1.004); only invalidating_writes, maintenance,
    maintenance_by_kind and slowest_commit changed.
(B) JetStream vs pg_ivm MVs, (C) PG memory: ../pg_ivm_sf20/data/paper_sweep/{write_rate_sweep,mem_footprint}.csv
    (GM rows, column workload_speedup; Total row stacked on the 35.5 GB of base tables).
Colors follow each sweep's own figure: blues from plot_rf_interval_sweep.py, purple read-only + greens from the
mvcmp combined plot; stateful navy from plot_memory_footprint.py.

Usage: plot_fig8.py OUT_DIR   (writes OUT_DIR/writerate_gm_duckdb_vs_pg_mv.{png,pdf})
"""
import csv
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.offsetbox import AnchoredOffsetbox, HPacker, TextArea

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plot_fig1 import read_only_geomeans  # noqa: E402

HERE = Path(__file__).resolve().parent
DUCK_SWEEP = HERE / "data" / "writerate" / "rf_interval_summary.csv"
PG_SWEEP = HERE.parent / "pg_ivm_sf20" / "data" / "paper_sweep"
READ_ONLY_COLOR = "#4b2a7b"
DUCK_RAMP = ["#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#0d366b"]
STATEFUL_COLOR = "#1A2654"
MV_COLOR = "#E0A030"
BASE_COLOR = "#d9d9d9"
BASE_TABLES_GB = 35.5
PANEL_LABEL_COLOR = "#B22222"


def short_rate(rows_s):
    return f"{rows_s / 1e3:.3g}k" if rows_s >= 1000 else f"{rows_s:.0f}"


def duckdb_points():
    logs, rows_s = {}, {}
    for r in csv.DictReader(open(DUCK_SWEEP)):
        T = float(r["interval_s"])
        if r["system"] == "base":
            rows_s[T] = float(r["rows_touched_per_s"])
        elif r["system"] == "cpp":
            logs.setdefault(T, []).append(math.log(float(r["workload_speedup_vs_duckdb"])))
    pts = [("RO", read_only_geomeans()["cpp"])]
    for T in sorted(logs, reverse=True):
        pts.append((short_rate(round(rows_s[T], -2)), math.exp(sum(logs[T]) / len(logs[T]))))
    return pts


def pg_points():
    pts = []
    for r in csv.DictReader(open(PG_SWEEP / "write_rate_sweep.csv")):
        if r["query"] == "GM":
            rate = r["rate"].replace(" rows/s", "").replace(",", "")
            pts.append(("RO" if rate == "Read-only" else short_rate(float(rate)), float(r["workload_speedup"])))
    return pts


def pg_memory_gb():
    total = next(r for r in csv.DictReader(open(PG_SWEEP / "mem_footprint.csv")) if r["query"] == "Total")
    return float(total["mv_gb"]), float(total["accel_gb"])


def speedup_panel(ax, pts, colors):
    xs = range(len(pts))
    ax.bar(xs, [gm for _, gm in pts], 0.75, color=colors)
    for x, (_, gm) in zip(xs, pts):
        ax.text(x, gm * 1.15, f"{gm:.1f}" if gm < 100 else f"{gm:.0f}", ha="center", va="bottom", rotation=90,
                fontsize=5)
    ax.axhline(1, color="gray", ls="--", lw=0.6)
    ax.set_yscale("log")
    ax.set_ylim(0.5, max(gm for _, gm in pts) * 20)
    ax.set_xticks(list(xs), [label for label, _ in pts], fontsize=5)
    ax.set_xlabel("Write rate (rows/s)")


def memory_panel(ax):
    mv, accel = pg_memory_gb()
    ax.bar([0, 1], BASE_TABLES_GB, 0.75, color=BASE_COLOR)
    ax.bar([0, 1], [mv, accel], 0.75, bottom=BASE_TABLES_GB, color=[MV_COLOR, STATEFUL_COLOR])
    for x, gb in enumerate([mv, accel]):
        ax.text(x, BASE_TABLES_GB + gb + 2, f"{BASE_TABLES_GB + gb:.0f}", ha="center", va="bottom", fontsize=5)
        ax.text(x, BASE_TABLES_GB / 2, "base", ha="center", va="center", rotation=90, fontsize=5)
    ax.set_ylim(0, (BASE_TABLES_GB + mv) * 1.3)
    ax.set_xticks([0, 1], ["MV", "Acc."], fontsize=5)
    ax.set_ylabel("Memory (GB)", labelpad=1)


def headline(ax, tag, title, center):
    """Colored panel tag and title as one group above the axes, centered left of middle to clear the panel on the right."""
    size = plt.rcParams["axes.titlesize"]
    group = HPacker(children=[TextArea(f"({tag})", textprops={"fontsize": size, "fontweight": "bold",
                                                                "color": PANEL_LABEL_COLOR}),
                              TextArea(title, textprops={"fontsize": size})], sep=2, pad=0)
    ax.add_artist(AnchoredOffsetbox(loc="lower center", child=group, frameon=False, pad=0, borderpad=0.6,
                                    bbox_to_anchor=(center, 1), bbox_transform=ax.transAxes))


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.size": 6, "axes.linewidth": 0.5, "xtick.major.width": 0.5, "ytick.major.width": 0.5,
                         "xtick.major.size": 2, "ytick.major.size": 2, "ytick.minor.size": 1,
                         "xtick.major.pad": 1.5, "ytick.major.pad": 1.5, "axes.titlesize": 6, "axes.titlepad": 3,
                         "axes.labelpad": 2, "pdf.fonttype": 42, "ps.fonttype": 42})
    duck, pg = duckdb_points(), pg_points()
    fig, (left, mid, mem) = plt.subplots(1, 3, figsize=(3.5, 1.4), gridspec_kw={"width_ratios": [6.4, 6, 4.6]})
    speedup_panel(left, duck, [READ_ONLY_COLOR] + DUCK_RAMP)
    speedup_panel(mid, pg, [READ_ONLY_COLOR] + list(plt.cm.Greens(np.linspace(0.45, 0.9, len(pg) - 1))))
    left.set_ylabel("Geomean speedup", labelpad=1)
    memory_panel(mem)
    for ax, tag, title, center in zip((left, mid, mem), "ABC", ("JetStream vs DuckDB", "JetStream vs PG MVs", "PG Memory"),
                                      (0.38, 0.38, 0.5)):
        headline(ax, tag, title, center)
    fig.tight_layout(pad=0.2, w_pad=1.0)
    x0, y0, w, h = mid.get_position().bounds
    mid.set_position([x0 - 0.03, y0, w, h])  # more room before the memory panel's y label
    for ext in ("png", "pdf"):
        fig.savefig(out / f"writerate_gm_duckdb_vs_pg_mv.{ext}", dpi=600, bbox_inches="tight", pad_inches=0.02)
    for name, pts in (("A (vs DuckDB)", duck), ("B (vs pg_ivm MVs)", pg)):
        print(f"{name}: " + ", ".join(f"{label} {gm:.1f}" for label, gm in pts))
    mv, accel = pg_memory_gb()
    print(f"C (PG memory): MV {BASE_TABLES_GB + mv:.0f} GB, Acc. {BASE_TABLES_GB + accel:.0f} GB")


if __name__ == "__main__":
    main()
