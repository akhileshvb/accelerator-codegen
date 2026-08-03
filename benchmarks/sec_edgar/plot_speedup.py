#!/usr/bin/env python3
"""Bar chart of SEC EDGAR SF3 single-threaded speedup by query (accelerator vs GenDB)."""

from __future__ import annotations

import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = Path(__file__).resolve().parent / "results"
DEFAULT_CSV = RESULTS_DIR / "speedup_summary.csv"
OUT_PNG = RESULTS_DIR / "sec_edgar_speedup.png"

QUERIES = ["Q1", "Q2", "Q3", "Q4", "Q6", "Q24"]

ACCEL_COLOR = "#4C72B0"
GENDDB_COLOR = "#DD8452"


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values))


def load_csv(path: Path) -> tuple[list[str], list[float], list[float]]:
    queries: list[str] = []
    speedups: list[float] = []
    gendb_speedups: list[float] = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            queries.append(row["query"])
            speedups.append(float(row["speedup_median"]))
            gendb_speedups.append(float(row["gendb_speedup_median"]))
    return queries, speedups, gendb_speedups


def plot(
    queries: list[str],
    speedups: list[float],
    gendb_speedups: list[float],
    out_path: Path,
) -> None:
    accel_geo = geomean(speedups)
    gendb_geo = geomean(gendb_speedups)

    x = np.arange(len(queries))
    bar_width = 0.36
    accel_offsets = x - bar_width / 2
    gendb_offsets = x + bar_width / 2

    fig, ax = plt.subplots(figsize=(9, 5.5), facecolor="white")

    accel_bars = ax.bar(
        accel_offsets,
        speedups,
        width=bar_width,
        color=ACCEL_COLOR,
        edgecolor="white",
        linewidth=0.6,
        label="Accelerator",
    )
    gendb_bars = ax.bar(
        gendb_offsets,
        gendb_speedups,
        width=bar_width,
        color=GENDDB_COLOR,
        edgecolor="white",
        linewidth=0.6,
        label="GenDB",
    )

    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.0, zorder=0)

    for bars in (accel_bars, gendb_bars):
        for bar in bars:
            value = bar.get_height()
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 0.08,
                f"{value:.1f}×",
                ha="center",
                va="bottom",
                fontsize=9,
                fontweight="medium",
            )

    ax.set_xticks(x)
    ax.set_xticklabels(queries, fontsize=11)
    ax.set_ylabel("Median speedup vs DuckDB (×)")
    ax.set_xlabel("Query")
    ymax = max(max(speedups), max(gendb_speedups))
    ax.set_ylim(0, ymax * 1.22)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.legend(loc="upper right", fontsize=10)
    ax.set_title(
        f"SEC EDGAR speedup (SF3, single-threaded, no aux state)\n"
        f"Geomean — Accelerator {accel_geo:.2f}× · GenDB {gendb_geo:.2f}×",
        fontsize=12,
        pad=12,
    )
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, bbox_inches="tight", facecolor="white")
    print(f"Wrote {out_path}")


def main() -> None:
    if not DEFAULT_CSV.is_file():
        raise SystemExit(f"Missing {DEFAULT_CSV}; run benchmarks first or create the CSV.")
    queries, speedups, gendb_speedups = load_csv(DEFAULT_CSV)
    plot(queries, speedups, gendb_speedups, OUT_PNG)


if __name__ == "__main__":
    main()
