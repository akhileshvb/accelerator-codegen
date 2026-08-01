#!/usr/bin/env python3
"""Plot cross-accelerator writer-scheduled sweep workload speedup (Part 2 aggregate)."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt

WRITE_FREQS = (0, 1, 10, 50)


def load_workload_by_freq(csv_path: Path) -> dict[int, tuple[float, float]]:
    """Return freq -> (workload_speedup, effective_write_pct)."""
    by_freq: dict[int, tuple[float, float]] = {}
    with csv_path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            freq = int(float(row["scheduled_write_pct"]))
            if freq in by_freq:
                continue
            by_freq[freq] = (float(row["workload_speedup"]), float(row["effective_write_pct"]))
    return by_freq


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "csv",
        nargs="?",
        default="build-release/concurrent_experiments/cross_accel_writer_scheduled_sweep.csv",
    )
    parser.add_argument("--output", type=Path, default=Path("build-release/concurrent_experiments_report/part2_workload_speedup.png"))
    args = parser.parse_args()

    by_freq = load_workload_by_freq(Path(args.csv))
    freqs = [f for f in WRITE_FREQS if f in by_freq]
    workloads = [by_freq[f][0] for f in freqs]
    effective = [by_freq[f][1] for f in freqs]

    fig, ax = plt.subplots(figsize=(8, 5), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    bars = ax.bar(
        [str(f) + "%" for f in freqs],
        workloads,
        color=["#2ca02c", "#98df8a", "#ffbb78", "#ff7f0e"][: len(freqs)],
        edgecolor="#333",
        linewidth=0.8,
    )
    for bar, wl, eff in zip(bars, workloads, effective):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 0.3,
            f"{wl:.1f}×\n({eff:.1f}% eff)",
            ha="center",
            va="bottom",
            fontsize=10,
            fontweight="bold",
        )

    ax.set_xlabel("Scheduled write frequency", fontsize=12)
    ax.set_ylabel("Workload speedup (geom read p50 × write adjustment)", fontsize=11)
    ax.set_title(
        "Part 2: Cross-accelerator workload speedup\n"
        "(6 readers, 1 writer, round-robin invalidating UPDATEs)",
        fontsize=12,
        fontweight="bold",
    )
    ax.grid(True, axis="y", linestyle=":", alpha=0.45)
    fig.tight_layout()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.output, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
