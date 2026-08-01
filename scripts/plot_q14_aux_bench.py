#!/usr/bin/env python3
"""Plot Q14 aux benchmark policy comparison and hybrid oracle regret."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

POLICIES = [
    "hybrid",
    "force_invalidate",
    "force_incremental",
    "force_rebuild",
    "table",
    "relevant",
    "incremental",
    "rebuild",
]
WRITE_FREQUENCIES = [0, 1, 10]

SUITE_LABELS = {
    "lineitem_only": "Lineitem (row-wise)",
    "part_only": "Part (row-wise)",
    "bulk_lineitem_month": "Bulk lineitem (month)",
    "bulk_part_month": "Bulk part (50 parts)",
}


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def format_bar_label(value: float) -> str:
    if value >= 10:
        return f"{value:.0f}×"
    return f"{value:.2f}×"


def annotate_bar_group(ax, bars: list, values: list[float]) -> None:
    ylim = ax.get_ylim()
    y_span = ylim[1] - ylim[0]
    stagger_step = y_span * 0.04
    group_max = max(values) if values else 0.0
    use_stagger = group_max < y_span * 0.2

    for index, (bar, value) in enumerate(zip(bars, values)):
        x = bar.get_x() + bar.get_width() / 2
        y = bar.get_height() + (index * stagger_step if use_stagger else 0.0)
        ax.text(x, y, format_bar_label(value), ha="center", va="bottom", fontsize=12)


def plot_policy_comparison(rows: list[dict[str, str]], suites: list[str], write_frequency_pct: int, out_path: Path) -> None:
    metric = "workload_speedup"
    policies = [policy for policy in POLICIES if any(row["write_policy"] == policy for row in rows)]
    if not policies:
        policies = POLICIES

    x = np.arange(len(suites))
    width = min(0.8 / max(len(policies), 1), 0.17)
    fig, ax = plt.subplots(figsize=(14, 7))

    all_bars_by_suite: list[list] = [[] for _ in suites]
    all_values_by_suite: list[list[float]] = [[] for _ in suites]

    for index, policy in enumerate(policies):
        values = []
        for suite in suites:
            match = [
                row
                for row in rows
                if row["suite"] == suite
                and row["write_policy"] == policy
                and int(row["write_frequency_pct"]) == write_frequency_pct
            ]
            values.append(float(match[0][metric]) if match else 0.0)

        offset = (index - (len(policies) - 1) / 2) * width
        bars = ax.bar(
            x + offset,
            values,
            width,
            label=policy,
            color=f"C{index % 10}",
            edgecolor="white",
            linewidth=0.6,
        )
        for suite_index, bar in enumerate(bars):
            all_bars_by_suite[suite_index].append(bar)
            all_values_by_suite[suite_index].append(values[suite_index])

    ax.set_xticks(x)
    ax.set_xticklabels(
        [SUITE_LABELS.get(suite, suite) for suite in suites],
        fontsize=13,
        rotation=12,
        ha="right",
    )
    ax.set_ylabel("Workload speedup vs DuckDB", fontsize=14)
    ax.set_title(f"Q14 write policy comparison — {write_frequency_pct}% writes (SF1)", fontsize=16, pad=14)
    ax.axhline(1.0, color="black", linestyle="--", linewidth=0.8, alpha=0.5)
    ax.grid(True, axis="y", alpha=0.3)
    ax.tick_params(axis="y", labelsize=12)
    ax.margins(y=0.18)
    ax.legend(
        loc="upper center",
        bbox_to_anchor=(0.5, -0.14),
        ncol=min(4, len(policies)),
        frameon=False,
        fontsize=11,
    )

    for group_bars, group_values in zip(all_bars_by_suite, all_values_by_suite):
        annotate_bar_group(ax, group_bars, group_values)

    fig.subplots_adjust(left=0.10, right=0.98, top=0.92, bottom=0.30)
    fig.savefig(out_path, dpi=160, bbox_inches="tight", pad_inches=0.25)
    plt.close(fig)


def plot_oracle_regret(rows: list[dict[str, str]], suites: list[str], write_frequency_pct: int, out_path: Path) -> None:
    if "oracle_regret_pct" not in rows[0]:
        return

    x = np.arange(len(suites))
    values = []
    for suite in suites:
        match = [
            row
            for row in rows
            if row["suite"] == suite
            and row["write_policy"] == "oracle"
            and int(row["write_frequency_pct"]) == write_frequency_pct
        ]
        values.append(float(match[0]["oracle_regret_pct"]) if match else 0.0)

    fig, ax = plt.subplots(figsize=(10, 5))
    bars = ax.bar(x, values, color="C3", edgecolor="white", linewidth=0.6)
    ax.set_xticks(x)
    ax.set_xticklabels([SUITE_LABELS.get(suite, suite) for suite in suites], rotation=12, ha="right")
    ax.set_ylabel("Oracle regret (%)", fontsize=14)
    ax.set_title(f"Hybrid oracle regret — {write_frequency_pct}% writes", fontsize=16, pad=14)
    ax.grid(True, axis="y", alpha=0.3)
    for bar, value in zip(bars, values):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(), f"{value:.1f}%", ha="center", va="bottom")
    fig.savefig(out_path, dpi=160, bbox_inches="tight", pad_inches=0.25)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "build" / "q14_aux_bench_sf1_sweep_v4" / "results.csv",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output directory (defaults to CSV parent / plots)",
    )
    args = parser.parse_args()

    csv_path = args.csv.resolve()
    out_dir = args.out_dir.resolve() if args.out_dir else csv_path.parent / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(csv_path)
    if not rows:
        raise SystemExit(f"No rows found in {csv_path}")

    suites = sorted({row["suite"] for row in rows})
    for write_frequency_pct in WRITE_FREQUENCIES:
        out_path = out_dir / f"policy_comparison_{write_frequency_pct}pct.png"
        plot_policy_comparison(rows, suites, write_frequency_pct, out_path)
        print(f"Wrote {out_path}")

        regret_path = out_dir / f"oracle_regret_{write_frequency_pct}pct.png"
        plot_oracle_regret(rows, suites, write_frequency_pct, regret_path)
        if regret_path.exists():
            print(f"Wrote {regret_path}")


if __name__ == "__main__":
    main()
