#!/usr/bin/env python3
"""Plot cross-accelerator writer-scheduled sweep: speedup vs DuckDB by write %."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt

WRITE_FREQS = (0, 1, 10, 50)
QUERIES = (1, 3, 6, 10, 12, 14)
COLORS = {1: "#1f77b4", 3: "#17becf", 6: "#9467bd", 10: "#ff7f0e", 12: "#2ca02c", 14: "#d62728"}
MARKERS = {1: "o", 3: "P", 6: "v", 10: "s", 12: "^", 14: "D"}


def format_speedup(value: float) -> str:
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def load_speedups(csv_path: Path) -> dict[int, dict[int, float]]:
    by_query_freq: dict[int, dict[int, float]] = {q: {} for q in QUERIES}
    with csv_path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            q = int(row["query_num"])
            freq = int(float(row["scheduled_write_pct"]))
            speedup = float(row["speedup_p50"] or 0)
            if speedup > 0:
                by_query_freq.setdefault(q, {})[freq] = speedup
    return by_query_freq


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "csv",
        nargs="?",
        default="build-release/cross_accel_writer_scheduled_sweep.csv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build-release/cross_accel_writer_scheduled_speedup.png"),
    )
    parser.add_argument("--title", default="Cross-accelerator speedup vs DuckDB (single writer, scheduled writes)")
    args = parser.parse_args()

    csv_path = Path(args.csv)
    by_query_freq = load_speedups(csv_path)

    fig, ax = plt.subplots(figsize=(10, 6), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    for query in QUERIES:
        freqs = [f for f in WRITE_FREQS if f in by_query_freq.get(query, {})]
        if not freqs:
            continue
        speedups = [by_query_freq[query][f] for f in freqs]
        ax.plot(
            freqs,
            speedups,
            label=f"Q{query}",
            color=COLORS[query],
            marker=MARKERS[query],
            linewidth=2.2,
            markersize=9,
        )
        for freq, speedup in zip(freqs, speedups):
            ax.annotate(
                format_speedup(speedup),
                (freq, speedup),
                textcoords="offset points",
                xytext=(0, 8),
                ha="center",
                fontsize=7,
                color=COLORS[query],
            )

    ax.set_xlabel("Scheduled write frequency (%)", fontsize=12)
    ax.set_ylabel("Speedup vs concurrent DuckDB (p50 read latency)", fontsize=12)
    ax.set_title(args.title, fontsize=13, fontweight="bold")
    ax.set_xticks(list(WRITE_FREQS))
    ax.set_xticklabels([f"{f}%" for f in WRITE_FREQS])
    ax.axhline(1.0, color="#888888", linestyle="--", linewidth=1, label="DuckDB parity")
    ax.set_yscale("log")
    ax.grid(True, which="both", linestyle=":", alpha=0.45)
    ax.legend(loc="upper right", ncol=2, framealpha=0.92, fontsize=10)
    fig.tight_layout()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, bbox_inches="tight")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
