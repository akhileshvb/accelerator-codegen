#!/usr/bin/env python3
"""Plot time-based write interval sweep."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize_aux_workload_write_interval_sweep import load_rows

QUERIES = (1, 6, 12, 14)
COLORS = {1: "#1f77b4", 6: "#9467bd", 12: "#2ca02c", 14: "#d62728"}
MARKERS = {1: "o", 6: "v", 12: "^", 14: "D"}


def interval_label(iv: float) -> float:
    return 0.25 if iv <= 0 else iv


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="?", default="build-release/concurrent_experiments/aux_workload_write_interval_sweep.csv")
    parser.add_argument("--out-dir", type=Path, default=Path("build-release/concurrent_experiments_report"))
    args = parser.parse_args()

    rows = load_rows(Path(args.csv))
    by_query: dict[int, list[tuple[float, float, float]]] = {}
    for row in rows:
        q = int(row["query_num"])
        iv = float(row["write_interval_sec"])
        by_query.setdefault(q, []).append(
            (interval_label(iv), float(row["workload_speedup"]), float(row["read_speedup_p50"]))
        )
    for q in by_query:
        by_query[q].sort(key=lambda t: t[0])

    queries = sorted({int(row["query_num"]) for row in rows})
    colors = {1: "#1f77b4", 6: "#9467bd", 12: "#2ca02c", 14: "#d62728"}
    markers = {1: "o", 6: "v", 12: "^", 14: "D"}

    fig, axes = plt.subplots(1, 2, figsize=(14, 5.5), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    for ax, idx, title in zip(axes, (1, 2), ("Workload speedup", "Read p50 speedup")):
        ax.set_facecolor("#fafafa")
        for q in queries:
            pts = by_query.get(q)
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ys = [p[idx] for p in pts]
            ax.plot(
                xs,
                ys,
                label=f"Q{q}",
                color=colors.get(q, "#333"),
                marker=markers.get(q, "o"),
                linewidth=2.2,
                markersize=8,
            )
        ax.set_xscale("symlog", linthresh=1)
        ax.set_yscale("log")
        ax.axhline(1.0, color="#888", linestyle="--", linewidth=1)
        ax.set_xlabel("Write interval (seconds; 0.25 = no writes)")
        ax.set_ylabel(title)
        ax.set_title(title, fontweight="bold")
        ax.grid(True, which="both", linestyle=":", alpha=0.4)
        ax.legend(fontsize=9)
    fig.suptitle(
        "Wall-clock write interval sweep (invalidating UPDATE every N seconds)",
        fontsize=13,
        fontweight="bold",
        y=1.02,
    )
    fig.tight_layout()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    out = args.out_dir / "part4_write_interval_speedup.png"
    fig.savefig(out, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
