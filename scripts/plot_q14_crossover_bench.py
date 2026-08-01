#!/usr/bin/env python3
"""Plot Q14 hybrid crossover sweep results."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

FORCED_POLICIES = [
    "force_incremental",
    "force_rebuild",
    "force_rebuild_all",
    "force_invalidate",
]


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def numeric_sort_key(value: str) -> float:
    if value == "full_month":
        return 1e9
    if value in {"low", "medium", "high"}:
        return {"low": 1.0, "medium": 2.0, "high": 3.0}[value]
    if value == "all":
        return 1e6
    try:
        return float(value)
    except ValueError:
        return 0.0


def annotate_best_forced(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    grouped: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[(row["crossover_axis"], row["crossover_value"])].append(row)
    annotated = []
    for key, group in grouped.items():
        forced = [r for r in group if r["write_policy"] in FORCED_POLICIES]
        best = min(forced, key=lambda r: float(r["total_maintenance_overhead_us"]), default=None)
        best_action = best["write_policy"] if best else ""
        for row in group:
            row = dict(row)
            row["best_forced_action"] = best_action
            annotated.append(row)
    return annotated


def axis_values(axis_rows: list[dict[str, str]]) -> list[str]:
    return sorted({r["crossover_value"] for r in axis_rows}, key=numeric_sort_key)


def format_speedup_label(speedup: float) -> str:
    if speedup >= 100:
        return f"{speedup:.0f}×"
    if speedup >= 10:
        return f"{speedup:.1f}×"
    return f"{speedup:.2f}×"


LABEL_BBOX = {
    "boxstyle": "round,pad=0.25",
    "facecolor": "white",
    "edgecolor": "C0",
    "alpha": 0.95,
    "linewidth": 0.6,
}


def hybrid_label_offset(
    idx: int,
    count: int,
    yi: float,
    neighbor_ys: list[float],
) -> tuple[tuple[float, float], str, str]:
    nearest_other = min((abs(other - yi) for other in neighbor_ys), default=float("inf"))
    crowded = nearest_other < max(8.0, 0.12 * yi)

    if idx >= count - 2:
        return (-16, 8), "right", "center"
    if crowded or yi < 20:
        return (0, 22), "center", "bottom"
    if idx % 2 == 0:
        return (14, 12), "left", "bottom"
    return (-14, -16), "right", "top"


def annotate_hybrid_speedup_labels(
    ax: plt.Axes,
    x: np.ndarray,
    ys: list[float],
    *,
    other_series: list[list[float]] | None = None,
) -> None:
    other_series = other_series or []
    count = len(ys)
    for idx, (xi, yi) in enumerate(zip(x, ys)):
        if yi <= 0:
            continue
        neighbor_ys = [series[idx] for series in other_series if idx < len(series)]
        xytext, ha, va = hybrid_label_offset(idx, count, yi, neighbor_ys)
        ax.annotate(
            format_speedup_label(yi),
            (xi, yi),
            textcoords="offset points",
            xytext=xytext,
            ha=ha,
            va=va,
            fontsize=8,
            color="C0",
            bbox=LABEL_BBOX,
            zorder=6,
        )


def queries_per_write_to_write_frequency_pct(queries_per_write: str) -> float:
    return 100.0 / float(queries_per_write)


def plot_speedup_panel(
    ax: plt.Axes,
    axis_rows: list[dict[str, str]],
    values: list[str],
    *,
    xlabel: str,
    title: str = "Workload speedup vs DuckDB",
    label_hybrid_points: bool = False,
    x_coords: list[float] | None = None,
    x_ticklabels: list[str] | None = None,
) -> None:
    x = np.array(x_coords if x_coords is not None else list(range(len(values))), dtype=float)
    tick_labels = x_ticklabels if x_ticklabels is not None else values
    policies = ["hybrid"] + FORCED_POLICIES
    hybrid_ys: list[float] = []
    other_policy_ys: list[list[float]] = []
    for policy in policies:
        ys = []
        for value in values:
            match = [r for r in axis_rows if r["crossover_value"] == value and r["write_policy"] == policy]
            ys.append(float(match[0]["workload_speedup"]) if match else 0.0)
        if policy == "hybrid":
            hybrid_ys = ys
        else:
            other_policy_ys.append(ys)
        linewidth = 2.5 if policy == "hybrid" else 1.2
        ax.plot(x, ys, marker="o", label=policy, linewidth=linewidth, zorder=2 if policy == "hybrid" else 1)
    if label_hybrid_points:
        annotate_hybrid_speedup_labels(ax, x, hybrid_ys, other_series=other_policy_ys)
    ax.set_xticks(x)
    ax.set_xticklabels(tick_labels)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("speedup")
    ax.set_title(title)
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)


def plot_speedup_standalone(rows: list[dict[str, str]], axis: str, out_dir: Path) -> None:
    axis_rows = [r for r in rows if r["crossover_axis"] == axis]
    if not axis_rows:
        return
    values = axis_values(axis_rows)
    write_frequencies = [queries_per_write_to_write_frequency_pct(value) for value in values]
    write_frequency_labels = [f"{freq:g}%" for freq in write_frequencies]
    fig, ax = plt.subplots(1, 1, figsize=(10, 5.5))
    plot_speedup_panel(
        ax,
        axis_rows,
        values,
        xlabel="write frequency (% of queries with a write)",
        title="Q14 workload speedup vs DuckDB (point lineitem writes)",
        label_hybrid_points=True,
        x_coords=write_frequencies,
        x_ticklabels=write_frequency_labels,
    )
    fig.tight_layout()
    out_path = out_dir / f"crossover_{axis}_speedup.png"
    fig.savefig(out_path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out_path}")


def plot_axis(rows: list[dict[str, str]], axis: str, out_dir: Path) -> None:
    axis_rows = [r for r in rows if r["crossover_axis"] == axis]
    if not axis_rows:
        return
    values = axis_values(axis_rows)
    x = np.arange(len(values))

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    ax_actions, ax_speedup, ax_maint, ax_cross = axes.flatten()

    hybrid = {r["crossover_value"]: r for r in axis_rows if r["write_policy"] == "hybrid"}
    action_keys = [
        ("action_selected_apply_delta", "ApplyDelta"),
        ("action_selected_rebuild_partition", "RebuildPart"),
        ("action_selected_rebuild_all", "RebuildAll"),
        ("action_selected_invalidate", "Invalidate"),
    ]
    bottom = np.zeros(len(values))
    for key, label in action_keys:
        heights = [float(hybrid.get(v, {}).get(key, 0) or 0) for v in values]
        ax_actions.bar(x, heights, bottom=bottom, label=label)
        bottom += np.array(heights)
    ax_actions.set_xticks(x)
    ax_actions.set_xticklabels(values, rotation=30, ha="right")
    ax_actions.set_title(f"Hybrid action counts — {axis}")
    ax_actions.legend(fontsize=8)
    ax_actions.grid(True, axis="y", alpha=0.3)

    plot_speedup_panel(ax_speedup, axis_rows, values, xlabel=axis, title="Workload speedup vs DuckDB")

    hybrid_maint = [float(hybrid.get(v, {}).get("total_maintenance_overhead_us", 0) or 0) for v in values]
    ax_maint.plot(x, hybrid_maint, marker="o", label="hybrid", color="C0")
    best_forced = []
    for value in values:
        forced = [r for r in axis_rows if r["crossover_value"] == value and r["write_policy"] in FORCED_POLICIES]
        best_forced.append(
            min((float(r["total_maintenance_overhead_us"]) for r in forced), default=0.0) if forced else 0.0
        )
    ax_maint.plot(x, best_forced, marker="s", label="best forced", color="C3")
    ax_maint.set_xticks(x)
    ax_maint.set_xticklabels(values, rotation=30, ha="right")
    ax_maint.set_title("Maintenance overhead (us)")
    ax_maint.set_yscale("log")
    ax_maint.legend(fontsize=8)
    ax_maint.grid(True, alpha=0.3)

    for policy in FORCED_POLICIES:
        ys = []
        for value in values:
            match = [r for r in axis_rows if r["crossover_value"] == value and r["write_policy"] == policy]
            ys.append(float(match[0]["total_maintenance_overhead_us"]) if match else np.nan)
        ax_cross.plot(x, ys, marker=".", label=policy)
    ax_cross.plot(x, hybrid_maint, marker="o", label="hybrid", linewidth=2)
    ax_cross.set_xticks(x)
    ax_cross.set_xticklabels(values, rotation=30, ha="right")
    ax_cross.set_title("Forced policy maintenance crossover")
    ax_cross.set_yscale("log")
    ax_cross.legend(fontsize=7)
    ax_cross.grid(True, alpha=0.3)

    fig.suptitle(f"Q14 hybrid crossover — {axis}", fontsize=14)
    fig.tight_layout()
    out_path = out_dir / f"crossover_{axis}.png"
    fig.savefig(out_path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    rows = annotate_best_forced(load_rows(args.csv))
    axes = sorted({r["crossover_axis"] for r in rows})
    for axis in axes:
        plot_axis(rows, axis, args.out_dir)
    if "queries_per_write" in axes:
        plot_speedup_standalone(rows, "queries_per_write", args.out_dir)


if __name__ == "__main__":
    main()
