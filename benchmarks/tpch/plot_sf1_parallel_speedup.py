#!/usr/bin/env python3
"""Plot SF1 multithreaded accelerator speedup vs matched-thread DuckDB (22 queries)."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = Path(__file__).resolve().parent / "results"
DEFAULT_CSV = RESULTS_DIR / "sf1_parallel_22q" / "geomean_by_query.csv"
OUT_PNG = RESULTS_DIR / "sf1_parallel_speedup_22q.png"
OUT_CSV = RESULTS_DIR / "sf1_parallel_speedup_22q.csv"

DEFAULT_THREADS = [1, 2, 4, 8, 16]
THREAD_COLORS = ["#4C72B0", "#55A868", "#C44E52", "#8172B2", "#CCB974"]
QUERIES = [f"Q{i}" for i in range(1, 23)]
THREADS: list[int] = DEFAULT_THREADS


def load_table(csv_path: Path) -> dict[str, dict[int, float]]:
    table: dict[str, dict[int, float]] = {q: {} for q in QUERIES}
    with csv_path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            query = row["query"]
            if query.endswith("_validated"):
                continue
            if query not in table:
                continue
            threads = int(row["threads"])
            table[query][threads] = float(row["geomean_speedup"])
    return table


def resolve_threads(table: dict[str, dict[int, float]], requested: list[int]) -> list[int]:
    threads = [t for t in requested if all(t in table[q] for q in QUERIES)]
    if not threads:
        raise SystemExit(f"No complete thread column in {requested} (csv missing data)")
    return threads


def load_speedups(csv_path: Path, threads: list[int]) -> dict[str, list[float]]:
    table = load_table(csv_path)
    threads = resolve_threads(table, threads)
    global THREADS
    THREADS = threads
    return {q: [table[q][t] for t in threads] for q in QUERIES}


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values))


def write_csv(speedups: dict[str, list[float]]) -> None:
    with OUT_CSV.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["query", *[f"t={t}" for t in THREADS], "notes"])
        for query in QUERIES:
            note = ""
            if query == "Q17":
                note = "nonzero-part synthetic instances (10 seeds)"
            elif query == "Q11":
                note = "fraction=0.0001 matched DuckDB SQL"
            w.writerow([query, *[f"{v:.4f}" for v in speedups[query]], note])
        overall = [geomean([speedups[q][i] for q in QUERIES]) for i in range(len(THREADS))]
        w.writerow(["overall_geomean", *[f"{v:.4f}" for v in overall], ""])


def plot_grouped_bars(ax, speedups: dict[str, list[float]]) -> None:
    x = np.arange(len(QUERIES))
    n_threads = len(THREADS)
    group_width = 0.82
    bar_width = group_width / n_threads
    x_labels = QUERIES

    for thread_idx, thread_count in enumerate(THREADS):
        offsets = x - group_width / 2 + bar_width / 2 + thread_idx * bar_width
        values = [speedups[query][thread_idx] for query in QUERIES]
        ax.bar(
            offsets,
            values,
            width=bar_width,
            label=f"t={thread_count}",
            color=THREAD_COLORS[thread_idx],
            edgecolor="white",
            linewidth=0.4,
        )

    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.0, zorder=0)
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, rotation=45, ha="right", fontsize=9)
    ax.set_ylabel("Speedup vs matched-thread DuckDB (×)")
    ax.set_xlabel("Query")
    ax.set_ylim(0.0, 8.5)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.legend(title="Threads", loc="upper left", ncol=5, fontsize=9, title_fontsize=9)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV, help="geomean_by_query.csv from matrix run")
    parser.add_argument(
        "--threads",
        type=str,
        default="",
        help="Comma-separated thread counts (default: auto 1,2,4,8,16 present in csv)",
    )
    args = parser.parse_args()

    requested = DEFAULT_THREADS if not args.threads else [int(x) for x in args.threads.split(",")]
    speedups = load_speedups(args.csv, requested)
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    write_csv(speedups)

    overall = [geomean([speedups[q][i] for q in QUERIES]) for i in range(len(THREADS))]

    fig, ax = plt.subplots(figsize=(18, 6.5), facecolor="white")
    plot_grouped_bars(ax, speedups)

    subtitle = (
        "Overall geomean: "
        + " · ".join(f"t={t}: {overall[i]:.2f}×" for i, t in enumerate(THREADS))
        + "  |  SF1 tpch_sf1.duckdb  |  warmup=3 iters=5 seeds=5  |  paired-alternate timing"
        + "  |  Q11 uses TPC-H fraction=0.0001 (matched DuckDB SQL)"
    )
    fig.suptitle(
        "TPC-H SF1 Parallel Accelerator Speedup vs Matched-Thread DuckDB",
        fontsize=14,
        fontweight="bold",
        y=0.98,
    )
    fig.text(0.5, 0.01, subtitle, ha="center", fontsize=9, color="#444444")

    fig.savefig(OUT_PNG, dpi=150, bbox_inches="tight", facecolor="white")
    print(f"Wrote {OUT_PNG}")
    print(f"Wrote {OUT_CSV}")
    print("Overall geomean:", " · ".join(f"t={t}: {overall[i]:.2f}×" for i, t in enumerate(THREADS)))


if __name__ == "__main__":
    main()
