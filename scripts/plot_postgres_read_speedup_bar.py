#!/usr/bin/env python3
"""Bar chart: Postgres aux read speedup vs base PostgreSQL (read-only)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from summarize_aux_workload_write_interval_sweep import load_rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "csv",
        nargs="?",
        default=str(ROOT / "build/concurrent_experiments/postgres_aux_workload_write_interval_sweep.csv"),
    )
    parser.add_argument("--out", type=Path, default=ROOT / "benchmarks/postgres/results/postgres_read_speedup_vs_base_pg.png")
    args = parser.parse_args()

    rows = load_rows(Path(args.csv))
    read_only = {
        int(row["query_num"]): float(row["read_speedup_p50"])
        for row in rows
        if float(row["write_interval_sec"]) == 0.0
    }
    if 10 not in read_only or 24 not in read_only:
        raise SystemExit("CSV missing read-only (write_interval=0) rows for Q10 and Q24")

    labels = ["TPC-H Q10", "SEC EDGAR Q24"]
    queries = [10, 24]
    speedups = [read_only[q] for q in queries]
    colors = ["#1f77b4", "#8c564b"]

    fig, ax = plt.subplots(figsize=(6, 4.5))
    bars = ax.bar(labels, speedups, color=colors, width=0.55, edgecolor="black", linewidth=0.8)

    ax.set_ylabel("Read latency speedup vs base PostgreSQL (p50)")
    ax.set_title("Postgres in-memory aux vs base PostgreSQL (read-only, 4 readers, 45s)")
    ax.set_yscale("log")
    ax.set_ylim(max(1.0, min(speedups) * 0.7), max(speedups) * 1.4)
    ax.grid(axis="y", which="both", linestyle="--", alpha=0.35)
    ax.axhline(1.0, color="gray", linewidth=1, linestyle="-", alpha=0.6)

    for bar, value in zip(bars, speedups):
        label = f"{value:.1f}×" if value >= 10 else f"{value:.2f}×"
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() * 1.08,
            label,
            ha="center",
            va="bottom",
            fontsize=11,
            fontweight="bold",
        )

    fig.tight_layout()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out, dpi=150, bbox_inches="tight")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
