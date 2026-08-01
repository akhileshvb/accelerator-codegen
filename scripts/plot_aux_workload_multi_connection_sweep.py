#!/usr/bin/env python3
"""Plot per-query multi-connection workload sweep (Part 1)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from merge_workload_sweep_csv import load_rows

WRITE_FREQS = (0, 1, 10, 50)
QUERIES = (1, 3, 6, 12, 14)
COLORS = {1: "#1f77b4", 3: "#17becf", 6: "#9467bd", 12: "#2ca02c", 14: "#d62728"}
MARKERS = {1: "o", 3: "P", 6: "v", 12: "^", 14: "D"}


def format_speedup(value: float) -> str:
    if value <= 0:
        return "—"
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def load_by_query_freq(csv_path: Path) -> dict[int, dict[int, dict]]:
    rows = load_rows(csv_path)
    out: dict[int, dict[int, dict]] = {}
    for (q_str, freq_str), row in rows.items():
        q = int(q_str)
        if q not in QUERIES:
            continue
        out.setdefault(q, {})[int(float(freq_str))] = row
    return out


def plot_metric(
    *,
    by_query_freq: dict[int, dict[int, dict]],
    metric: str,
    ylabel: str,
    title: str,
    output: Path,
    log_scale: bool = False,
) -> None:
    fig, ax = plt.subplots(figsize=(10, 6), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    for query in QUERIES:
        freqs = [f for f in WRITE_FREQS if f in by_query_freq.get(query, {})]
        if not freqs:
            continue
        values = [float(by_query_freq[query][f][metric]) for f in freqs]
        ax.plot(
            freqs,
            values,
            label=f"Q{query}",
            color=COLORS[query],
            marker=MARKERS[query],
            linewidth=2.2,
            markersize=9,
        )
        for freq, value in zip(freqs, values):
            if value <= 0:
                continue
            ax.annotate(
                format_speedup(value),
                (freq, value),
                textcoords="offset points",
                xytext=(0, 8),
                ha="center",
                fontsize=7,
                color=COLORS[query],
            )

    ax.set_xlabel("Scheduled write frequency (%)", fontsize=12)
    ax.set_ylabel(ylabel, fontsize=12)
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.set_xticks(list(WRITE_FREQS))
    ax.set_xticklabels([f"{f}%" for f in WRITE_FREQS])
    if log_scale:
        ax.set_yscale("log")
        ax.axhline(1.0, color="#888888", linestyle="--", linewidth=1, label="DuckDB parity")
    ax.grid(True, which="both", linestyle=":", alpha=0.45)
    ax.legend(loc="upper right", ncol=2, framealpha=0.92, fontsize=10)
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {output}")


def plot_combined(by_query_freq: dict[int, dict[int, dict]], output: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    panels = [
        (axes[0], "workload_speedup", "Workload speedup (read + write)"),
        (axes[1], "read_speedup_p50", "Read p50 speedup (latency only)"),
    ]
    for ax, metric, subtitle in panels:
        ax.set_facecolor("#fafafa")
        for query in QUERIES:
            freqs = [f for f in WRITE_FREQS if f in by_query_freq.get(query, {})]
            if not freqs:
                continue
            values = [float(by_query_freq[query][f][metric]) for f in freqs]
            ax.plot(
                freqs,
                values,
                label=f"Q{query}",
                color=COLORS[query],
                marker=MARKERS[query],
                linewidth=2.0,
                markersize=8,
            )
        ax.set_xlabel("Scheduled write %")
        ax.set_ylabel(subtitle)
        ax.set_title(subtitle, fontsize=11, fontweight="bold")
        ax.set_xticks(list(WRITE_FREQS))
        ax.set_xticklabels([f"{f}%" for f in WRITE_FREQS])
        ax.set_yscale("log")
        ax.axhline(1.0, color="#888888", linestyle="--", linewidth=1)
        ax.grid(True, which="both", linestyle=":", alpha=0.4)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, -0.02), ncol=5, fontsize=9, framealpha=0.9)
    fig.suptitle(
        "Part 1: Per-query multi-connection workload (N readers + 1 writer per query)",
        fontsize=13,
        fontweight="bold",
        y=1.02,
    )
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "csv",
        nargs="?",
        default="build-release/concurrent_experiments/aux_workload_multi_connection_sweep.csv",
    )
    parser.add_argument("--out-dir", type=Path, default=Path("build-release/concurrent_experiments_report"))
    args = parser.parse_args()

    csv_path = Path(args.csv)
    by_query_freq = load_by_query_freq(csv_path)

    plot_combined(by_query_freq, args.out_dir / "part1_workload_and_read_speedup.png")
    plot_metric(
        by_query_freq=by_query_freq,
        metric="workload_speedup",
        ylabel="Workload speedup (baseline / accel, read + write)",
        title="Part 1: Workload speedup by query and write %",
        output=args.out_dir / "part1_workload_speedup.png",
        log_scale=True,
    )
    plot_metric(
        by_query_freq=by_query_freq,
        metric="read_speedup_p50",
        ylabel="Read p50 speedup (baseline / accel)",
        title="Part 1: Read p50 speedup by query and write %",
        output=args.out_dir / "part1_read_p50_speedup.png",
        log_scale=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
