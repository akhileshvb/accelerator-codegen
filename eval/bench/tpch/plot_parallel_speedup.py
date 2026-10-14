#!/usr/bin/env python3
"""Plot multithreaded accelerator speedup vs matched-thread DuckDB (22 queries), SF1 or SF10.

  python3 eval/bench/tpch/plot_parallel_speedup.py --sf 1|10 [--csv geomean_by_query.csv] [--threads 1,2,4,8,16]
                                                   [--out-dir DIR]

Reads results/sf<SF>_parallel_22q/geomean_by_query.csv (matrix run) and writes sf<SF>_parallel_speedup_22q.{png,csv}.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = Path(__file__).resolve().parent / "results"

DEFAULT_THREADS = [1, 2, 4, 8, 16]
THREAD_COLORS = ["#4C72B0", "#55A868", "#C44E52", "#8172B2", "#CCB974"]
QUERIES = [f"Q{i}" for i in range(1, 23)]

# Per-scale-factor differences: footnote, the Q17 label, extra CSV notes, whether the y axis grows with the data.
SF_CONFIG = {
    1: {
        "subtitle": "SF1 tpch_sf1.duckdb  |  warmup=3 iters=5 seeds=5  |  paired-alternate timing"
                    "  |  Q11 uses TPC-H fraction=0.0001 (matched DuckDB SQL)",
        "q17_star": False, "notes": {}, "auto_ylim": False,
    },
    10: {
        "subtitle": "SF10 tpch_sf10.duckdb  |  warmup=3 iters=5 seeds=5{thread_note}"
                    "  |  Q17* = 10 nonzero brand/container pairs  |  Q11 fraction=0.0001",
        "q17_star": True, "auto_ylim": True,
        "notes": {"Q5": "t=8 post-opt uint8 dense order index", "Q7": "t=8 post-opt shipdate/supplier-first probe"},
    },
}


def geomean(values: list[float]) -> float:
    return math.exp(sum(math.log(v) for v in values) / len(values))


def load_table(csv_path: Path) -> dict[str, dict[int, float]]:
    table: dict[str, dict[int, float]] = {q: {} for q in QUERIES}
    with csv_path.open(newline="") as f:
        for row in csv.DictReader(f):
            query = row["query"]
            if query.endswith("_validated") or query not in table:
                continue
            table[query][int(row["threads"])] = float(row["geomean_speedup"])
    return table


def resolve_threads(table: dict[str, dict[int, float]], requested: list[int]) -> list[int]:
    threads = [t for t in requested if all(t in table[q] for q in QUERIES)]
    if not threads:
        raise SystemExit(f"No complete thread column in {requested} (csv missing data)")
    return threads


def write_csv(path: Path, cfg: dict, speedups: dict[str, list[float]], threads: list[int]) -> None:
    notes = {"Q17": "nonzero-part synthetic instances (10 seeds)", "Q11": "fraction=0.0001 matched DuckDB SQL",
             **cfg["notes"]}
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["query", *[f"t={t}" for t in threads], "notes"])
        for query in QUERIES:
            w.writerow([query, *[f"{v:.4f}" for v in speedups[query]], notes.get(query, "")])
        overall = [geomean([speedups[q][i] for q in QUERIES]) for i in range(len(threads))]
        w.writerow(["overall_geomean", *[f"{v:.4f}" for v in overall], ""])


def plot_grouped_bars(ax, cfg: dict, speedups: dict[str, list[float]], threads: list[int]) -> None:
    x = np.arange(len(QUERIES))
    group_width = 0.82
    bar_width = group_width / len(threads)
    x_labels = [("Q17*" if q == "Q17" and cfg["q17_star"] else q) for q in QUERIES]
    y_cap = 8.5
    if cfg["auto_ylim"]:
        y_cap = max(y_cap, math.ceil(max(max(speedups[q]) for q in QUERIES) * 1.15 * 2) / 2)

    for thread_idx, thread_count in enumerate(threads):
        offsets = x - group_width / 2 + bar_width / 2 + thread_idx * bar_width
        ax.bar(offsets, [speedups[query][thread_idx] for query in QUERIES], width=bar_width,
               label=f"t={thread_count}", color=THREAD_COLORS[thread_idx], edgecolor="white", linewidth=0.4)

    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.0, zorder=0)
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, rotation=45, ha="right", fontsize=9)
    ax.set_ylabel("Speedup vs matched-thread DuckDB (×)")
    ax.set_xlabel("Query")
    ax.set_ylim(0.0, y_cap)
    ax.grid(axis="y", linestyle=":", alpha=0.4)
    ax.legend(title="Threads", loc="upper left", ncol=len(threads), fontsize=9, title_fontsize=9)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sf", type=int, choices=sorted(SF_CONFIG), required=True)
    parser.add_argument("--csv", type=Path, default=None, help="geomean_by_query.csv from matrix run")
    parser.add_argument("--threads", type=str, default="",
                        help="Comma-separated thread counts (default: auto 1,2,4,8,16 present in csv)")
    parser.add_argument("--out-dir", type=Path, default=RESULTS_DIR)
    args = parser.parse_args()
    cfg = SF_CONFIG[args.sf]
    csv_path = args.csv or RESULTS_DIR / f"sf{args.sf}_parallel_22q" / "geomean_by_query.csv"
    out_png = args.out_dir / f"sf{args.sf}_parallel_speedup_22q.png"
    out_csv = args.out_dir / f"sf{args.sf}_parallel_speedup_22q.csv"

    table = load_table(csv_path)
    requested = DEFAULT_THREADS if not args.threads else [int(x) for x in args.threads.split(",")]
    threads = resolve_threads(table, requested)
    speedups = {q: [table[q][t] for t in threads] for q in QUERIES}
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(out_csv, cfg, speedups, threads)

    overall = [geomean([speedups[q][i] for q in QUERIES]) for i in range(len(threads))]
    overall_s = " · ".join(f"t={t}: {overall[i]:.2f}×" for i, t in enumerate(threads))

    fig, ax = plt.subplots(figsize=(18, 6.5), facecolor="white")
    plot_grouped_bars(ax, cfg, speedups, threads)

    thread_note = "" if threads == DEFAULT_THREADS else f"  |  threads={','.join(str(t) for t in threads)}"
    subtitle = f"Overall geomean: {overall_s}  |  " + cfg["subtitle"].format(thread_note=thread_note)
    fig.suptitle(f"TPC-H SF{args.sf} Parallel Accelerator Speedup vs Matched-Thread DuckDB",
                 fontsize=14, fontweight="bold", y=0.98)
    fig.text(0.5, 0.01, subtitle, ha="center", fontsize=9, color="#444444")

    fig.savefig(out_png, dpi=150, bbox_inches="tight", facecolor="white")
    print(f"Wrote {out_png}")
    print(f"Wrote {out_csv}")
    print("Overall geomean:", overall_s)


if __name__ == "__main__":
    main()
