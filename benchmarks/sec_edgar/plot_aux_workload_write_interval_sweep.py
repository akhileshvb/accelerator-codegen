#!/usr/bin/env python3
"""Plot SEC EDGAR time-based write interval sweep."""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from summarize_aux_workload_write_interval_sweep import load_rows

COLORS = {1: "#1f77b4", 2: "#ff7f0e", 3: "#2ca02c", 4: "#d62728", 6: "#9467bd", 24: "#8c564b"}
MARKERS = {1: "o", 2: "s", 3: "^", 4: "D", 6: "v", 24: "P"}
ANNOTATION_FONTSIZE = 10
MIN_LOG_LABEL_GAP = 0.18
DEFAULT_CSV = ROOT / "build-release/concurrent_experiments/sec_edgar_aux_workload_write_interval_sweep.csv"
DEFAULT_OUT = Path(__file__).resolve().parent / "results" / "sec_edgar_aux_write_interval_speedup.png"


def format_speedup(value: float) -> str:
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def spread_log_positions(values: list[float], min_log_gap: float) -> list[float]:
    if not values:
        return []
    if len(values) == 1:
        return values[:]
    log_values = [math.log10(v) for v in values]
    lo, hi = min(log_values), max(log_values)
    min_span = min_log_gap * (len(values) - 1)
    span = max(hi - lo, min_span)
    center = (lo + hi) / 2
    start = center - span / 2
    step = span / (len(values) - 1)
    return [10 ** (start + i * step) for i in range(len(values))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="?", default=str(DEFAULT_CSV))
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT.parent)
    args = parser.parse_args()

    rows = load_rows(Path(args.csv))
    by_query: dict[int, list[tuple[float, float]]] = {}
    no_writes: dict[int, float] = {}
    for row in rows:
        q = int(row["query_num"])
        iv = float(row["write_interval_sec"])
        speedup = float(row["workload_speedup"])
        if iv <= 0:
            no_writes[q] = speedup
            continue
        by_query.setdefault(q, []).append((iv, speedup))
    for q in by_query:
        by_query[q].sort(key=lambda t: t[0])

    queries = sorted(set(by_query) | set(no_writes))
    min_labels: list[tuple[int, float, float, float]] = []

    fig, ax = plt.subplots(figsize=(10, 5.5), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    for q in queries:
        color = COLORS.get(q, "#333")
        marker = MARKERS.get(q, "o")
        pts = by_query.get(q, [])
        if pts:
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            ax.plot(
                xs,
                ys,
                label=f"Q{q}",
                color=color,
                marker=marker,
                linewidth=2.2,
                markersize=8,
            )
            left_iv, left_speedup = pts[0]
            min_speedup = min(y for _, y in pts)
            min_labels.append((q, left_iv, left_speedup, min_speedup))
        if q in no_writes:
            ax.axhline(
                no_writes[q],
                color=color,
                linestyle=":",
                linewidth=1.8,
                alpha=0.85,
            )

    ax.set_xscale("symlog", linthresh=1)
    ax.set_yscale("log")
    ax.axhline(1.0, color="#888", linestyle="--", linewidth=1)

    min_labels.sort(key=lambda item: item[3])
    label_ys = spread_log_positions([item[3] for item in min_labels], MIN_LOG_LABEL_GAP)
    for index, (q, left_iv, left_speedup, min_speedup) in enumerate(min_labels):
        color = COLORS.get(q, "#333")
        ax.annotate(
            format_speedup(min_speedup),
            xy=(left_iv, left_speedup),
            xytext=(left_iv * 0.55, label_ys[index]),
            fontsize=ANNOTATION_FONTSIZE,
            color=color,
            fontweight="bold",
            ha="right",
            va="center",
            arrowprops={
                "arrowstyle": "-",
                "color": color,
                "linewidth": 0.9,
                "alpha": 0.75,
                "shrinkA": 2,
                "shrinkB": 2,
            },
            bbox={"boxstyle": "round,pad=0.2", "facecolor": "white", "edgecolor": color, "alpha": 0.9},
        )

    ax.set_xlabel("Write interval (seconds)")
    ax.set_ylabel("Workload speedup")
    ax.set_title("Workload speedup", fontweight="bold")
    ax.grid(True, which="both", linestyle=":", alpha=0.4)
    ax.legend(fontsize=9, title="Solid = with writes\nDotted = no writes")
    fig.suptitle(
        "SEC EDGAR aux state speedup vs DuckDB (SF3) — wall-clock write interval sweep",
        fontsize=13,
        fontweight="bold",
        y=1.02,
    )
    fig.tight_layout()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    out = args.out_dir / "sec_edgar_aux_write_interval_speedup.png"
    fig.savefig(out, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
