#!/usr/bin/env python3
"""Plot Q14 phased hybrid workload timeline."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt

ACTION_COLORS = {
    "ApplyDelta": "C0",
    "RebuildPartition": "C1",
    "RebuildAll": "C2",
    "InvalidateAndRebuildOnRead": "C3",
    "NoOp": "C4",
    "": "lightgray",
}


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def query_speedup_series(query_rows: list[dict[str, str]]) -> tuple[list[int], list[float]] | None:
    if not query_rows:
        return None
    if "query_speedup" in query_rows[0]:
        return (
            [int(r["global_query_index"]) for r in query_rows],
            [float(r["query_speedup"]) for r in query_rows],
        )
    duckdb_median = float(query_rows[0].get("duckdb_query_median_us", 0) or 0)
    if duckdb_median <= 0:
        return None
    qx = [int(r["global_query_index"]) for r in query_rows]
    speedups = [
        duckdb_median / float(r["query_latency_us"])
        if float(r["query_latency_us"]) > 0
        else 0.0
        for r in query_rows
    ]
    return qx, speedups


def plot_phase_bands_panel(
    ax: plt.Axes,
    rows: list[dict[str, str]],
    write_rows: list[dict[str, str]],
    query_rows: list[dict[str, str]],
    *,
    show_xlabel: bool = False,
    legend_below: bool = False,
) -> None:
    phases = sorted({r["phase_id"] for r in rows})
    phase_colors = {pid: f"C{i}" for i, pid in enumerate(phases)}
    global_x = [int(r["global_query_index"]) for r in write_rows]
    colors = [ACTION_COLORS.get(r["selected_action"], "gray") for r in write_rows]

    for start_row in [r for r in rows if r["event_type"] == "phase_start"]:
        phase = start_row["phase_id"]
        x0 = int(start_row["global_query_index"])
        phase_rows = [r for r in rows if r["phase_id"] == phase]
        x1 = int(phase_rows[-1]["global_query_index"]) if phase_rows else x0
        ax.axvspan(x0, x1, alpha=0.15, color=phase_colors[phase], label=f"Phase {phase}")

    ax.scatter(global_x, [1] * len(global_x), c=colors, s=60, zorder=3)
    ax.set_yticks([])
    ax.set_ylabel("writes")

    ax_speedup = ax.twinx()
    speedup_series = query_speedup_series(query_rows)
    if speedup_series:
        qx, speedups = speedup_series
        ax_speedup.plot(qx, speedups, color="black", linewidth=1.5, label="speedup vs DuckDB")
        ax_speedup.set_ylabel("speedup vs DuckDB")
        ax_speedup.grid(False)

    ax.set_title("Phase bands, selected action at writes, and query speedup vs DuckDB")
    handles_left, labels_left = ax.get_legend_handles_labels()
    handles_right, labels_right = ax_speedup.get_legend_handles_labels()
    legend_kwargs = {"fontsize": 8, "frameon": True}
    if legend_below:
        ax.legend(
            handles_left + handles_right,
            labels_left + labels_right,
            loc="upper center",
            bbox_to_anchor=(0.5, -0.22),
            ncol=5,
            **legend_kwargs,
        )
    else:
        ax.legend(
            handles_left + handles_right,
            labels_left + labels_right,
            loc="upper left",
            ncol=2,
            **legend_kwargs,
        )
    if show_xlabel:
        ax.set_xlabel("global query index")


def plot_phased(rows: list[dict[str, str]], out_dir: Path) -> None:
    write_rows = [r for r in rows if r["event_type"] == "write"]
    if not write_rows:
        print("no write events in phased timeline")
        return

    query_rows = [r for r in rows if r["event_type"] == "query"]

    fig_top, ax_top = plt.subplots(1, 1, figsize=(14, 4.5))
    plot_phase_bands_panel(ax_top, rows, write_rows, query_rows, show_xlabel=True, legend_below=True)
    fig_top.subplots_adjust(bottom=0.28)
    top_path = out_dir / "phased_phase_bands.png"
    fig_top.savefig(top_path, dpi=160, bbox_inches="tight")
    plt.close(fig_top)
    print(f"Wrote {top_path}")

    fig, axes = plt.subplots(4, 1, figsize=(14, 12), sharex=True)

    plot_phase_bands_panel(axes[0], rows, write_rows, query_rows)

    ax1 = axes[1]
    global_x = [int(r["global_query_index"]) for r in write_rows]
    ax1.plot(global_x, [float(r["predicted_apply_delta_us"]) for r in write_rows], label="ApplyDelta", marker="o")
    ax1.plot(global_x, [float(r["predicted_rebuild_partition_us"]) for r in write_rows], label="RebuildPart", marker="o")
    ax1.plot(global_x, [float(r["predicted_rebuild_all_us"]) for r in write_rows], label="RebuildAll", marker="o")
    ax1.plot(global_x, [float(r["predicted_invalidate_us"]) for r in write_rows], label="Invalidate", marker="o")
    ax1.set_ylabel("predicted total us")
    ax1.set_yscale("log")
    ax1.set_title("Predicted action costs at writes")
    ax1.legend(fontsize=8)
    ax1.grid(True, alpha=0.3)

    ax2 = axes[2]
    qx = [int(r["global_query_index"]) for r in query_rows]
    ax2.plot(qx, [float(r["accel_hit_rate_so_far"]) for r in query_rows], label="hit rate")
    ax2.plot(qx, [float(r["ewma_query_rate_eps"]) for r in query_rows], label="query rate eps")
    ax2.plot(qx, [float(r["ewma_write_rate_eps"]) for r in query_rows], label="write rate eps")
    ax2.set_ylabel("rate / hit")
    ax2.set_title("EWMA rates and aux hit rate")
    ax2.legend(fontsize=8)
    ax2.grid(True, alpha=0.3)

    ax3 = axes[3]
    ax3.plot(global_x, [float(r["actual_maintenance_us"]) for r in write_rows], marker="o", label="actual maint")
    ax3.plot(global_x, [float(r["predicted_selected_us"]) for r in write_rows], marker=".", label="predicted selected")
    ax3.set_xlabel("global query index")
    ax3.set_ylabel("us")
    ax3.set_title("Actual vs predicted maintenance overhead")
    ax3.legend(fontsize=8)
    ax3.grid(True, alpha=0.3)

    fig.tight_layout()
    out_path = out_dir / "phased_timeline.png"
    fig.savefig(out_path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    plot_phased(load_rows(args.csv), args.out_dir)


if __name__ == "__main__":
    main()
