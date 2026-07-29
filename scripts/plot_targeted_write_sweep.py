#!/usr/bin/env python3
"""Plot cross-accelerator targeted-write isolation sweep results."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LogNorm

QUERY_COLORS = {
    1: "#2563eb",
    3: "#7c3aed",
    10: "#db2777",
    12: "#059669",
    14: "#d97706",
}

MODE_LABELS = {
    "A-idle": "A idle",
    "B-round-robin": "B round-robin",
    "C-target-Q1": "C → Q1",
    "C-target-Q3": "C → Q3",
    "C-target-Q10": "C → Q10",
    "C-target-Q12": "C → Q12",
    "C-target-Q14": "C → Q14",
}

MODE_ORDER = list(MODE_LABELS.keys())
QUERY_ORDER = [1, 3, 10, 12, 14]


def load_meta(csv_path: Path) -> dict[str, dict[int, dict[str, float]]]:
    meta: dict[str, dict[int, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))
    with csv_path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            label = row["sweep_label"]
            query = int(row["query_num"])
            meta[label][query] = {
                "speedup": float(row["speedup_p50"]),
                "accel_p50": float(row["accel_p50_us"]),
                "effective_write_pct": float(row["effective_write_pct"]),
                "writer_commits": float(row["accel_writer_commits"]),
            }
    return meta


def load_speedups(csv_path: Path) -> dict[str, dict[int, float]]:
    meta = load_meta(csv_path)
    return {mode: {q: vals["speedup"] for q, vals in queries.items()} for mode, queries in meta.items()}


def plot_grouped_bars(by_mode: dict[str, dict[int, float]], out_path: Path, title: str) -> None:
    modes = [m for m in MODE_ORDER if m in by_mode]
    n_modes = len(modes)
    n_queries = len(QUERY_ORDER)
    x = np.arange(n_modes)
    width = 0.15

    fig, ax = plt.subplots(figsize=(14, 7), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    for i, query in enumerate(QUERY_ORDER):
        offsets = x + (i - (n_queries - 1) / 2) * width
        values = [by_mode[mode].get(query, np.nan) for mode in modes]
        bars = ax.bar(
            offsets,
            values,
            width=width,
            label=f"Q{query}",
            color=QUERY_COLORS[query],
            edgecolor="white",
            linewidth=0.6,
            zorder=3,
        )
        for bar, value in zip(bars, values):
            if np.isnan(value) or value <= 0:
                continue
            label = f"{value:.0f}×" if value >= 20 else f"{value:.1f}×"
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() * 1.08,
                label,
                ha="center",
                va="bottom",
                fontsize=11,
                fontweight="bold",
                color="black",
            )

    ax.axhline(1.0, color="#9ca3af", linestyle="--", linewidth=1.2, zorder=2, label="Parity (1×)")
    ax.set_yscale("log")
    ax.set_ylabel("Speedup vs concurrent DuckDB SQL (p50, log scale)", fontsize=11)
    ax.set_xlabel("Writer mode", fontsize=11)
    ax.set_title(title, fontsize=13, fontweight="bold", pad=12)
    ax.set_xticks(x)
    ax.set_xticklabels([MODE_LABELS[m] for m in modes], rotation=20, ha="right")
    ax.set_ylim(1, 1200)
    ax.grid(axis="y", which="major", linestyle=":", alpha=0.45, zorder=0)
    ax.legend(loc="upper left", ncol=6, frameon=True, fontsize=9)
    fig.tight_layout()
    fig.savefig(out_path, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)


def plot_delta_from_idle(by_mode: dict[str, dict[int, float]], out_path: Path, title_suffix: str) -> None:
    idle = by_mode.get("A-idle", {})
    modes = [m for m in MODE_ORDER if m in by_mode and m != "A-idle"]
    n_modes = len(modes)
    n_queries = len(QUERY_ORDER)
    x = np.arange(n_modes)
    width = 0.15

    fig, ax = plt.subplots(figsize=(14, 6), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    for i, query in enumerate(QUERY_ORDER):
        offsets = x + (i - (n_queries - 1) / 2) * width
        values = []
        for mode in modes:
            base = idle.get(query, np.nan)
            cur = by_mode[mode].get(query, np.nan)
            if base and base > 0 and cur and not np.isnan(cur):
                values.append(100.0 * (cur / base - 1.0))
            else:
                values.append(np.nan)
        ax.bar(offsets, values, width=width, label=f"Q{query}", color=QUERY_COLORS[query])

    ax.axhline(0.0, color="#6b7280", linestyle="--", linewidth=1.2)
    ax.set_ylabel("% change in speedup vs A-idle", fontsize=11)
    ax.set_xlabel("Writer mode", fontsize=11)
    ax.set_title("Speedup delta from A-idle\n" + title_suffix, fontsize=13, fontweight="bold", pad=12)
    ax.set_xticks(x)
    ax.set_xticklabels([MODE_LABELS[m] for m in modes], rotation=20, ha="right")
    ax.grid(axis="y", linestyle=":", alpha=0.45)
    ax.legend(loc="upper right", ncol=5, fontsize=9)
    fig.tight_layout()
    fig.savefig(out_path, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)


def plot_write_rate_comparison(low_csv: Path, high_csv: Path, out_path: Path, high_label: str) -> None:
    low = load_meta(low_csv)
    high = load_meta(high_csv)
    modes = [m for m in MODE_ORDER if m in low and m in high]

    fig, axes = plt.subplots(1, len(QUERY_ORDER), figsize=(16, 5), dpi=150, sharey=True)
    fig.patch.set_facecolor("#fafafa")
    fig.suptitle(f"Low vs high write rate (speedup p50)\nHigh: {high_label}", fontsize=13, fontweight="bold")

    x = np.arange(len(modes))
    for ax, query in zip(axes, QUERY_ORDER):
        low_vals = [max(low[m][query]["speedup"], 0.01) for m in modes]
        high_vals = [max(high[m][query]["speedup"], 0.01) for m in modes]
        ax.bar(x - 0.2, low_vals, width=0.4, label="sleep=200ms", color="#93c5fd")
        ax.bar(x + 0.2, high_vals, width=0.4, label="sleep=0ms", color="#1d4ed8")
        ax.set_yscale("log")
        ax.set_ylim(1, 1200)
        ax.set_title(f"Q{query}", fontweight="bold")
        ax.set_xticks(x)
        ax.set_xticklabels([MODE_LABELS[m].replace("C → ", "C") for m in modes], fontsize=6, rotation=45, ha="right")
        ax.axhline(1.0, color="#9ca3af", linestyle="--", linewidth=1)
        ax.grid(axis="y", linestyle=":", alpha=0.35)

    axes[0].set_ylabel("Speedup (log)")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout()
    fig.savefig(out_path, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="?", default="build-release/cross_accel_targeted_write_sweep.csv")
    parser.add_argument("--high-csv", default="", help="Optional second CSV for write-rate comparison")
    parser.add_argument("--high-label", default="writer_sleep_ms=0, bulk_rows=1")
    parser.add_argument("--out-dir", default="build-release")
    parser.add_argument("--title", default="Cross-accelerator targeted-write isolation")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = Path(args.csv)
    by_mode = load_speedups(csv_path)

    plot_grouped_bars(by_mode, out_dir / "cross_accel_targeted_write_sweep.png", args.title)
    plot_delta_from_idle(by_mode, out_dir / "cross_accel_targeted_write_sweep_delta.png", args.title)

    if args.high_csv:
        plot_write_rate_comparison(
            csv_path,
            Path(args.high_csv),
            out_dir / "cross_accel_targeted_write_sweep_write_rate_compare.png",
            args.high_label,
        )

    print(f"Wrote plots to {out_dir}")


if __name__ == "__main__":
    main()
