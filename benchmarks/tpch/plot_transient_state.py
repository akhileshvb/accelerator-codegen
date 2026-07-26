#!/usr/bin/env python3
"""Generate static plot: query-local transient state — accelerator vs DuckDB."""

from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = Path(__file__).resolve().parent / "results"
OUT_PNG = RESULTS_DIR / "tpch_transient_state_plot.png"
OUT_CSV = RESULTS_DIR / "tpch_transient_state.csv"

# Accelerator: kernel structures + scale_performance_report / stage reports.
# DuckDB: analytical estimates from hash-join / hash-agg plan shapes (not profiled RSS).
DATA = [
    # query, accel_sf1, accel_sf10, duck_sf1, duck_sf10, measured_accel
    ("Q1", 12, 12, 0.1, 0.1, False),
    ("Q2", 15, 130, 50, 480, False),
    ("Q3", 27, 320, 20, 150, True),
    ("Q4", 0.75, 12, 35, 280, True),
    ("Q5", 2, 20, 25, 180, True),
    ("Q6", 0.01, 0.01, 0.1, 0.5, True),
    ("Q7", 7, 60, 20, 150, True),
    ("Q8", 12, 100, 50, 420, False),
    ("Q9", 40, 280, 60, 450, True),
    ("Q10", 90, 650, 40, 280, False),
    ("Q11", 4, 35, 35, 280, False),
    ("Q12", 6, 60, 25, 200, True),
    ("Q13", 1, 10, 15, 120, False),
    ("Q14", 0.2, 2, 8, 50, True),
    ("Q15", 1, 8, 15, 100, False),
    ("Q16", 12, 200, 80, 650, False),
    ("Q17", 3.4, 3.4, 12, 12, True),
    ("Q18", 50, 500, 35, 280, False),
    ("Q19", 0.2, 0.2, 5, 5, True),
    ("Q20", 35, 220, 45, 380, False),
    ("Q21", 18, 140, 75, 650, False),
    ("Q22", 3, 30, 12, 100, False),
]

QUERIES = [r[0] for r in DATA]
ACCEL_SF1 = np.array([r[1] for r in DATA])
ACCEL_SF10 = np.array([r[2] for r in DATA])
DUCK_SF1 = np.array([r[3] for r in DATA])
DUCK_SF10 = np.array([r[4] for r in DATA])


def write_csv() -> None:
    with OUT_CSV.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "query",
                "accel_sf1_mb",
                "accel_sf10_mb",
                "duckdb_sf1_mb_est",
                "duckdb_sf10_mb_est",
                "ratio_sf1",
                "ratio_sf10",
                "accel_measured",
            ]
        )
        for row in DATA:
            q, a1, a10, d1, d10, measured = row
            w.writerow([q, a1, a10, d1, d10, a1 / d1, a10 / d10, measured])


def plot_bars(ax, accel: np.ndarray, duck: np.ndarray, title: str) -> None:
    x = np.arange(len(QUERIES))
    width = 0.36
    ax.bar(x - width / 2, accel, width, label="Accelerator", color="#4C72B0", edgecolor="white", linewidth=0.5)
    ax.bar(x + width / 2, duck, width, label="DuckDB (est.)", color="#DD8452", edgecolor="white", linewidth=0.5)
    ax.set_yscale("log")
    ax.set_ylabel("Transient memory (MB, log scale)")
    ax.set_title(title, fontsize=12, fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(QUERIES, rotation=45, ha="right", fontsize=8)
    ax.grid(axis="y", which="both", linestyle=":", alpha=0.5)
    ax.set_ylim(bottom=0.005)


def main() -> None:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    write_csv()

    accel_wins_sf1 = int(np.sum(ACCEL_SF1 < DUCK_SF1))
    accel_wins_sf10 = int(np.sum(ACCEL_SF10 < DUCK_SF10))

    fig, axes = plt.subplots(2, 1, figsize=(14, 9), constrained_layout=True)

    plot_bars(axes[0], ACCEL_SF1, DUCK_SF1, "SF1 — estimated working set per query")
    plot_bars(axes[1], ACCEL_SF10, DUCK_SF10, "SF10 — estimated working set per query")

    handles, labels = axes[0].get_legend_handles_labels()
    axes[0].legend(handles, labels, loc="upper left", fontsize=9)

    note = (
        f"Accelerator wins on memory: SF1 {accel_wins_sf1}/22 · SF10 {accel_wins_sf10}/22  |  "
        f"Totals SF1: accel {ACCEL_SF1.sum():.0f} MB vs DuckDB {DUCK_SF1.sum():.0f} MB  |  "
        f"SF10: accel {ACCEL_SF10.sum():.0f} MB vs DuckDB {DUCK_SF10.sum():.0f} MB"
    )
    fig.text(0.5, -0.01, note, ha="center", fontsize=9, color="#444444")

    fig.savefig(OUT_PNG, dpi=150, bbox_inches="tight", facecolor="white")
    print(f"Wrote {OUT_PNG}")
    print(f"Wrote {OUT_CSV}")


if __name__ == "__main__":
    main()
