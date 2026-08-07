#!/usr/bin/env python3
"""Plot Postgres aux combined sweep: read-only bar chart and write-interval line plot."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CSV = ROOT / "benchmarks/aux_state/results/postgres_aux_combined_sweep.csv"
DEFAULT_OUT_DIR = ROOT / "benchmarks/aux_state/results"

WORKLOAD_COLORS = {"tpch": "#2563eb", "sec_edgar": "#ea580c"}
WORKLOAD_LABELS = {"tpch": "TPC-H", "sec_edgar": "SEC EDGAR"}


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def query_label(workload: str, query: str) -> str:
    prefix = WORKLOAD_LABELS.get(workload, workload)
    return f"{prefix}\nQ{query}"


def plot_read_only_bars(rows: list[dict[str, str]], out_path: Path) -> None:
    read_only = [
        r
        for r in rows
        if r["mode"] == "shared" and float(r["write_interval_sec"]) == 0.0 and r["read_speedup_p50"]
    ]
    read_only.sort(key=lambda r: (r["workload"], float(r["query"])))

    labels = [query_label(r["workload"], r["query"]) for r in read_only]
    speedups = [float(r["read_speedup_p50"]) for r in read_only]
    colors = [WORKLOAD_COLORS[r["workload"]] for r in read_only]

    fig, ax = plt.subplots(figsize=(max(12, len(labels) * 0.9), 6), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    x = np.arange(len(labels))
    bars = ax.bar(x, speedups, color=colors, edgecolor="white", linewidth=0.8)
    ax.set_yscale("log")
    ax.set_ylabel("Read p50 speedup vs base (log scale)")
    ax.set_xlabel("Query")
    ax.set_title(
        "Postgres Query Accelerators with Aux State (Read-Only Speedup)",
        fontweight="bold",
    )
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    ax.axhline(1.0, color="#888", linestyle="--", linewidth=1)
    ax.grid(True, axis="y", which="both", linestyle=":", alpha=0.4)

    for bar, value in zip(bars, speedups):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() * 1.08,
            f"{value:.0f}×" if value >= 10 else f"{value:.1f}×",
            ha="center",
            va="bottom",
            fontsize=7,
            rotation=90 if value >= 100 else 0,
        )

    handles = [
        plt.Rectangle((0, 0), 1, 1, color=WORKLOAD_COLORS[w])
        for w in ("tpch", "sec_edgar")
    ]
    ax.legend(
        handles,
        [WORKLOAD_LABELS[w] for w in ("tpch", "sec_edgar")],
        loc="upper center",
        bbox_to_anchor=(0.5, -0.14),
        ncol=2,
        frameon=True,
    )

    fig.tight_layout()
    fig.subplots_adjust(bottom=0.22)
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_write_interval_lines(rows: list[dict[str, str]], out_path: Path) -> None:
    shared = [r for r in rows if r["mode"] == "shared"]
    series: dict[tuple[str, str], list[tuple[float, float]]] = {}
    for row in shared:
        key = (row["workload"], row["query"])
        interval = float(row["write_interval_sec"])
        speedup = float(row["workload_speedup_vs_base"])
        series.setdefault(key, []).append((interval, speedup))
    for pts in series.values():
        pts.sort(key=lambda t: t[0])

    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), dpi=150, sharey=True)
    fig.patch.set_facecolor("#fafafa")

    for ax, workload in zip(axes, ("tpch", "sec_edgar")):
        ax.set_facecolor("#fafafa")
        keys = sorted([k for k in series if k[0] == workload], key=lambda k: float(k[1]))
        cmap = plt.get_cmap("tab10")
        for idx, key in enumerate(keys):
            pts = series[key]
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            ax.plot(
                xs,
                ys,
                marker="o",
                linewidth=2.0,
                markersize=7,
                color=cmap(idx % 10),
                label=f"Q{key[1]}",
            )
        ax.set_yscale("log")
        ax.axhline(1.0, color="#888", linestyle="--", linewidth=1)
        ax.set_xlabel("Write interval (seconds)")
        ax.set_ylabel("Workload speedup vs base (log scale)")
        ax.set_title(WORKLOAD_LABELS[workload], fontweight="bold")
        ax.set_xticks([0.0, 0.5, 1.0, 2.0])
        ax.grid(True, which="both", linestyle=":", alpha=0.4)
        ax.legend(fontsize=8, ncol=2)

    fig.suptitle(
        "Postgres aux state: workload speedup vs write interval",
        fontsize=13,
        fontweight="bold",
        y=1.02,
    )
    fig.tight_layout()
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    args = parser.parse_args()

    if not args.csv.exists():
        raise SystemExit(f"missing csv: {args.csv} (run build_postgres_aux_combined_sweep.py first)")

    rows = load_rows(args.csv)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    plot_read_only_bars(rows, args.out_dir / "postgres_aux_read_only_speedup.png")
    plot_write_interval_lines(rows, args.out_dir / "postgres_aux_write_interval_speedup.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
