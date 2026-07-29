#!/usr/bin/env python3
"""Plot aux-state workload speedup vs DuckDB across write-frequency sweep points."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt

DEFAULT_BUILD_DIR = Path(__file__).resolve().parents[1] / "build-release"
QUERIES = (1, 6, 10, 12, 14)
WRITE_FREQS = (0, 1, 10, 50)
COLORS = {1: "#1f77b4", 6: "#9467bd", 10: "#ff7f0e", 12: "#2ca02c", 14: "#d62728"}
MARKERS = {1: "o", 6: "v", 10: "s", 12: "^", 14: "D"}


def format_speedup(value: float) -> str:
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def load_speedups(results_csv: Path, metric_col: str) -> dict[int, float]:
    by_freq: dict[int, float] = {}
    with results_csv.open(newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            if not row.get("write_frequency_pct"):
                continue
            freq = int(float(row["write_frequency_pct"]))
            by_freq[freq] = float(row[metric_col])
    return by_freq


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help="CMake build directory containing q*_aux_bench_sf1/results.csv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "benchmarks/tpch/results/aux_write_freq_speedup.png",
        help="Output image path",
    )
    parser.add_argument(
        "--metric",
        choices=("workload", "query_only"),
        default="query_only",
        help="Speedup column to plot (default: workload_speedup)",
    )
    args = parser.parse_args()

    metric_col = "workload_speedup" if args.metric == "workload" else "query_only_speedup"
    metric_label = (
        "Workload speedup (queries + writes)"
        if args.metric == "workload"
        else "Query-only speedup (aux kernel vs DuckDB ref)"
    )

    fig, ax = plt.subplots(figsize=(9, 5.5))

    for query in QUERIES:
        results_csv = args.build_dir / f"q{query}_aux_bench_sf1" / "results.csv"
        if not results_csv.is_file():
            raise SystemExit(f"missing results: {results_csv}")

        by_freq = load_speedups(results_csv, metric_col)
        freqs = [f for f in WRITE_FREQS if f in by_freq]
        speedups = [by_freq[f] for f in freqs]

        ax.plot(
            freqs,
            speedups,
            label=f"Q{query}",
            color=COLORS[query],
            marker=MARKERS[query],
            linewidth=2,
            markersize=8,
        )

        for freq, speedup in zip(freqs, speedups):
            ax.annotate(
                format_speedup(speedup),
                (freq, speedup),
                ha="center",
                va="center",
                fontsize=7,
                color=COLORS[query],
                bbox={
                    "boxstyle": "round,pad=0.25",
                    "facecolor": "white",
                    "edgecolor": COLORS[query],
                    "linewidth": 1.2,
                    "alpha": 0.95,
                },
            )

    ax.set_xlabel("Write frequency (%)", fontsize=12)
    ax.set_ylabel(f"{metric_label} (×)", fontsize=12)
    ax.set_title("TPC-H Aux state speedup vs duckdb", fontsize=13)
    ax.set_xticks(list(WRITE_FREQS))
    ax.set_xticklabels([f"{f}%" for f in WRITE_FREQS])
    ax.axhline(1.0, color="#888888", linestyle="--", linewidth=1, label="DuckDB baseline")
    ax.set_yscale("log")
    ax.grid(True, which="both", linestyle=":", alpha=0.5)
    ax.legend(loc="best", framealpha=0.9)
    fig.tight_layout()
    fig.subplots_adjust(top=0.92)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, dpi=160)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
