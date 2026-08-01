#!/usr/bin/env python3
"""Bar chart of SEC EDGAR SF3 single-threaded accelerator median speedup by query."""

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


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values))


def load_csv(path: Path) -> tuple[list[str], list[float], list[float], list[float]]:
    queries: list[str] = []
    speedups: list[float] = []
    ref_ms: list[float] = []
    accel_ms: list[float] = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            queries.append(row["query"])
            speedups.append(float(row["speedup_median"]))
            ref_ms.append(float(row["reference_median_ms"]))
            accel_ms.append(float(row["accelerator_median_ms"]))
    return queries, speedups, ref_ms, accel_ms


def plot(queries: list[str], speedups: list[float], out_path: Path) -> None:
    geo = geomean(speedups)
    x = np.arange(len(queries))
    colors = ["#4C72B0" if s >= 1.0 else "#C44E52" for s in speedups]

    fig, ax = plt.subplots(figsize=(9, 5.5), facecolor="white")
    bars = ax.bar(x, speedups, color=colors, edgecolor="white", linewidth=0.6, width=0.65)
    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.0, zorder=0, label="1× DuckDB")

    for bar, value in zip(bars, speedups):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 0.04,
            f"{value:.2f}×",
            ha="center",
            va="bottom",
            fontsize=10,
            fontweight="medium",
        )

    ax.set_xticks(x)
    ax.set_xticklabels(queries, fontsize=11)
    ax.set_ylabel("Median speedup (×)")
    ax.set_xlabel("Query")
    ax.set_ylim(0, max(speedups) * 1.18)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.set_title(
        f"SEC EDGAR accelerator speedup (SF3, single-threaded)\n"
        f"Geomean {geo:.2f}× · DuckDB reference median / accelerator median",
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
    queries, speedups, _, _ = load_csv(DEFAULT_CSV)
    plot(queries, speedups, OUT_PNG)


if __name__ == "__main__":
    main()
