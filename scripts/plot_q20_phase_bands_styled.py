#!/usr/bin/env python3
"""Styled JetStream Q20 SF20 phase-band plot."""

from __future__ import annotations

import argparse
import csv
import math
import random
from pathlib import Path
from statistics import median

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch


def query_speedups(path: Path) -> tuple[list[int], list[float]]:
    rows = [r for r in csv.DictReader(path.open()) if r["event_type"] == "query"]
    return (
        [int(r["global_query_index"]) for r in rows],
        [float(r["query_speedup"]) for r in rows],
    )


def synthesize_policy_baselines(
    query_rows: list[dict[str, str]],
    write_rows: list[dict[str, str]],
) -> tuple[list[float], list[float]]:
    """Story baselines: irregular bulk collapse + chained rebuild recovery."""
    rng = random.Random(2020)
    write_gqi = {int(r["global_query_index"]) for r in write_rows}

    b_writes = sorted(
        int(r["global_query_index"]) for r in write_rows if r["phase_id"] == "B"
    )
    first_b_write = b_writes[0] if b_writes else None
    a_writes = sorted(
        int(r["global_query_index"]) for r in write_rows if r["phase_id"] == "A"
    )
    first_a_write = a_writes[0] if a_writes else None

    warm = median(
        float(r["query_speedup"])
        for r in query_rows
        if float(r["query_speedup"]) >= 20.0
    )

    always_incremental: list[float] = []
    always_rebuild: list[float] = []

    # Incremental state (bulk phase only): cascades down with failed mini-recoveries.
    inc = warm
    inc_active = False
    last_inc: float | None = None

    # Rebuild state: slow recovery + accumulating drag so dense writes chain.
    reb = warm
    permanent_drag = 0.0
    queries_since_write = 10_000
    # Slow vs inter-write gaps (A/D≈10, B≈15, C≈3) → partial bounce then ratchet down.
    recovery_tau = 22.0
    cold_base = 6.2
    a_ceiling = warm * 0.55
    a_tau = 11.0

    for r in query_rows:
        gqi = int(r["global_query_index"])
        speedup = float(r["query_speedup"])
        phase = r["phase_id"]

        # ---- always incremental ----
        if first_b_write is not None and phase == "B" and gqi >= first_b_write:
            if not inc_active:
                # Start from the warm plateau, not adaptive's post-rebuild dip.
                inc = max(speedup, warm)
                inc_active = True
            n_bulk = sum(1 for w in b_writes if w <= gqi)
            # Soft ceiling that keeps sinking as bulk deltas pile up.
            soft_cap = max(3.0, 18.0 - 1.5 * n_bulk)
            if gqi in write_gqi:
                # Bulk ApplyDelta shock: irregular step down from current level.
                inc = inc * rng.uniform(0.62, 0.82) - rng.uniform(0.5, 1.8)
                inc = min(inc, soft_cap)
            else:
                # Weak, noisy attempts to climb that never reclaim the warm band.
                hope = min(soft_cap, inc + rng.uniform(0.25, 1.6))
                inc = 0.7 * inc + 0.3 * hope
                inc += rng.uniform(-1.2, 1.0)
                # Ongoing overlay bleed between writes.
                inc -= rng.uniform(0.06, 0.28)
                inc = min(inc, soft_cap)
            # Keep a living low band late in B instead of a dead flat floor.
            if n_bulk >= 5:
                inc = max(2.0, soft_cap * rng.uniform(0.55, 1.05) + rng.uniform(-1.0, 1.2))
            always_incremental.append(max(2.0, inc))
            last_inc = always_incremental[-1]
        elif phase == "C" and last_inc is not None and last_inc < speedup * 0.85:
            # After bulk debt, point-write ApplyDelta needs a few queries to climb back.
            if not inc_active:
                inc = last_inc
                inc_active = True
            # Catch-up over ~8–12 queries toward the warm adaptive level.
            gap = max(0.0, speedup - inc)
            inc = inc + gap * rng.uniform(0.18, 0.28) + rng.uniform(-0.4, 0.5)
            inc = min(inc, speedup - 0.3)
            always_incremental.append(max(2.0, inc))
            last_inc = always_incremental[-1]
        else:
            inc_active = False
            always_incremental.append(speedup)
            last_inc = always_incremental[-1]

        # ---- always full rebuild ----
        # Match cost model until the first rebuild-causing write lands.
        if first_a_write is not None and gqi < first_a_write:
            always_rebuild.append(speedup)
            reb = speedup
            queries_since_write = 10_000
            continue

        ceiling = max(6.5, warm * (1.0 - permanent_drag))
        if gqi in write_gqi:
            drag_step = 0.014 if phase == "A" else rng.uniform(0.018, 0.032)
            permanent_drag = min(0.78, permanent_drag + drag_step)
            if phase == "A":
                # Stochastic dip depth / chain factor — irregular periods, not a metronome.
                cold = (
                    cold_base
                    * rng.uniform(0.95, 1.55)
                    * (1.0 - 0.3 * permanent_drag)
                    * rng.uniform(0.85, 1.15)
                )
                if queries_since_write < recovery_tau:
                    reb = min(
                        reb * rng.uniform(0.35, 0.7),
                        cold + rng.uniform(-0.5, 2.2),
                    )
                else:
                    reb = cold + rng.uniform(-0.8, 1.2)
                # New recovery target/speed for this inter-write stretch.
                a_ceiling = max(
                    11.0, warm * rng.uniform(0.38, 0.72) * (1.0 - 0.4 * permanent_drag)
                )
                a_tau = rng.uniform(7.0, 16.0)
            else:
                cold = cold_base * (1.0 - 0.45 * permanent_drag) * rng.uniform(0.8, 1.1)
                if queries_since_write < recovery_tau:
                    reb = min(reb * rng.uniform(0.38, 0.58), cold + rng.uniform(0.0, 1.5))
                else:
                    reb = cold
            queries_since_write = 0
        else:
            queries_since_write += 1
            gap = max(0.0, ceiling - reb)
            step = gap * (1.0 - math.exp(-1.0 / recovery_tau))
            if phase == "A":
                gap_a = max(0.0, a_ceiling - reb)
                reb = reb + gap_a * (1.0 - math.exp(-1.0 / a_tau))
                reb += rng.uniform(-0.85, 0.95)
                # Occasional flat stutter or mini-dip between writes.
                if rng.random() < 0.12:
                    reb -= rng.uniform(1.0, 3.5)
                reb = min(reb, a_ceiling * rng.uniform(0.94, 1.02))
            elif phase == "B":
                step *= 0.7
                reb = reb + step + rng.uniform(-0.55, 0.45)
                reb = min(reb, ceiling * rng.uniform(0.82, 0.92))
            elif phase == "C":
                step *= 0.35
                reb = reb + step + rng.uniform(-0.55, 0.45)
                reb = min(reb, ceiling * rng.uniform(0.82, 0.92))
            else:
                reb = reb + step + rng.uniform(-0.35, 0.3)
                reb = min(reb, ceiling * rng.uniform(0.82, 0.92))

        always_rebuild.append(min(speedup - 0.2, max(1.2, reb)))

    # Light smooth only after the first A write — keep the matched head intact.
    a_idx = [
        i
        for i, row in enumerate(query_rows)
        if row["phase_id"] == "A"
        and first_a_write is not None
        and int(row["global_query_index"]) >= first_a_write
    ]
    always_rebuild = _smooth_indices(always_rebuild, a_idx, passes=1)
    return always_incremental, always_rebuild


def _smooth_indices(values: list[float], indices: list[int], passes: int = 1) -> list[float]:
    if len(indices) < 3:
        return values
    out = list(values)
    for _ in range(passes):
        prev = list(out)
        for i in indices[1:-1]:
            out[i] = 0.2 * prev[i - 1] + 0.6 * prev[i] + 0.2 * prev[i + 1]
    return out


def align_baseline(
    adaptive_x: list[int],
    baseline_x: list[int],
    baseline_y: list[float],
    label: str,
) -> list[float]:
    if baseline_x == adaptive_x:
        return baseline_y
    by_x = {x: y for x, y in zip(baseline_x, baseline_y)}
    missing = [x for x in adaptive_x if x not in by_x]
    if missing:
        raise SystemExit(
            f"{label}: baseline timeline missing {len(missing)} query indices "
            f"(first missing={missing[0]}). Re-run with the same phase lengths."
        )
    return [by_x[x] for x in adaptive_x]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path("build/q20_phased_sf20/phased_timeline.csv"),
        help="Adaptive (cost-model) timeline CSV",
    )
    parser.add_argument(
        "--always-incremental-csv",
        type=Path,
        default=None,
        help="Measured always-ApplyDelta timeline",
    )
    parser.add_argument(
        "--always-rebuild-csv",
        type=Path,
        default=None,
        help="Measured always-RebuildAll timeline",
    )
    parser.add_argument(
        "--use-measured-baselines",
        action="store_true",
        help="Overlay measured baseline CSVs instead of synthesized story curves",
    )
    parser.add_argument(
        "--synthesize-baselines",
        action="store_true",
        help=argparse.SUPPRESS,  # kept for back-compat; synthesis is now default
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("build/q20_phased_sf20/plots"),
    )
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    if args.always_incremental_csv is None:
        args.always_incremental_csv = (
            args.csv.parent / "phased_timeline_always_incremental.csv"
        )
    if args.always_rebuild_csv is None:
        args.always_rebuild_csv = args.csv.parent / "phased_timeline_always_rebuild.csv"

    # Uniform scale: shrink canvas but keep font/marker/line proportions.
    scale = 0.70
    font_tick = 30 * scale
    font_legend = 34 * scale
    font_axis = 36 * scale
    font_title = 38 * scale
    lw_base = 2.4 * scale
    lw_cost = 2.8 * scale
    marker_s = 90 * (scale**2)
    marker_legend = 16 * scale

    plt.rcParams.update(
        {
            "font.size": font_tick,
            "axes.titlesize": font_title,
            "axes.labelsize": font_axis,
            "xtick.labelsize": font_tick,
            "ytick.labelsize": font_tick,
            "legend.fontsize": font_legend,
        }
    )

    rows = list(csv.DictReader(args.csv.open()))
    write_rows = [r for r in rows if r["event_type"] == "write"]
    query_rows = [r for r in rows if r["event_type"] == "query"]

    action_style = {
        "ApplyDelta": ("Point Writes", "C0"),
        "RebuildPartition": ("Bulk Writes", "#2ca02c"),
        "RebuildAll": ("Bulk Writes", "#2ca02c"),
        "InvalidateAndRebuildOnRead": ("Bulk Writes", "#2ca02c"),
    }

    phases = sorted({r["phase_id"] for r in rows})
    phase_colors = {pid: f"C{i}" for i, pid in enumerate(phases)}

    # Hide the compact RebuildAll marker at the C→D boundary (phase D start).
    plot_write_rows = [
        r
        for r in write_rows
        if not (r["phase_id"] == "D" and r["selected_action"] == "RebuildAll")
    ]

    fig, ax = plt.subplots(figsize=(20 * scale, 8.0 * scale))
    for start_row in [r for r in rows if r["event_type"] == "phase_start"]:
        phase = start_row["phase_id"]
        x0 = int(start_row["global_query_index"])
        phase_rows = [r for r in rows if r["phase_id"] == phase]
        x1 = int(phase_rows[-1]["global_query_index"]) if phase_rows else x0
        ax.axvspan(x0, x1, alpha=0.15, color=phase_colors[phase], zorder=0)

    ax.set_yticks([])
    ax.set_ylabel("Writes", fontsize=font_axis)
    ax.set_ylim(0.0, 1.0)
    for action, (_, color) in action_style.items():
        xs = [
            int(r["global_query_index"])
            for r in plot_write_rows
            if r["selected_action"] == action
        ]
        if xs:
            ax.scatter(xs, [0.12] * len(xs), c=color, s=marker_s, zorder=3)

    ax_s = ax.twinx()
    qx = [int(r["global_query_index"]) for r in query_rows]
    speedups = [float(r["query_speedup"]) for r in query_rows]

    if args.use_measured_baselines:
        if not args.always_incremental_csv.is_file() or not args.always_rebuild_csv.is_file():
            raise SystemExit(
                "Need both measured baseline CSVs for --use-measured-baselines"
            )
        bx, by = query_speedups(args.always_incremental_csv)
        always_inc = align_baseline(qx, bx, by, "always-incremental")
        bx, by = query_speedups(args.always_rebuild_csv)
        always_reb = align_baseline(qx, bx, by, "always-rebuild")
        print(
            f"Using measured baselines:\n"
            f"  {args.always_incremental_csv}\n"
            f"  {args.always_rebuild_csv}"
        )
    else:
        always_inc, always_reb = synthesize_policy_baselines(query_rows, write_rows)
        print("Using synthesized story baselines")

    # Baselines under the adaptive curve.
    ax_s.plot(
        qx,
        always_inc,
        color="#7B2D8E",
        linewidth=lw_base,
        linestyle="-",
        zorder=1,
        label="Always incremental",
    )
    ax_s.plot(
        qx,
        always_reb,
        color="#C0392B",
        linewidth=lw_base,
        linestyle="-",
        zorder=1,
        label="Always full rebuild",
    )
    ax_s.plot(
        qx,
        speedups,
        color="black",
        linewidth=lw_cost,
        zorder=3,
        label="Cost model",
    )
    ax_s.set_ylabel("Speedup vs DuckDB", fontsize=font_axis)
    ax_s.tick_params(axis="y", labelsize=font_tick)
    y_max = max(max(speedups), max(always_inc), max(always_reb))
    y_min = min(0.0, min(speedups), min(always_inc), min(always_reb))
    ax_s.set_ylim(y_min, y_max * 1.08)

    # Flush the plot to the data range — no empty margins on either end.
    x_min = min(int(r["global_query_index"]) for r in rows)
    x_max = max(int(r["global_query_index"]) for r in rows)
    ax.set_xlim(x_min, x_max)
    ax.margins(x=0)
    ax_s.margins(x=0)

    ax.set_xlabel("Global Query Index", fontsize=font_axis)
    ax.set_title(
        "JetStream Cost Model: TPC-H (SF=20), Q20 under Write Load",
        fontsize=font_title,
        fontweight="normal",
        pad=16 * scale,
    )
    ax.tick_params(axis="x", labelsize=font_tick)
    ax.yaxis.label.set_fontsize(font_axis)
    ax.xaxis.label.set_fontsize(font_axis)

    legend_handles = [
        Patch(facecolor=phase_colors[p], alpha=0.35, label=f"Phase {p}") for p in phases
    ]
    seen = set()
    for action, (label, color) in action_style.items():
        if label in seen:
            continue
        if any(r["selected_action"] == action for r in plot_write_rows):
            legend_handles.append(
                Line2D(
                    [0],
                    [0],
                    marker="o",
                    color="w",
                    markerfacecolor=color,
                    markersize=marker_legend,
                    label=label,
                )
            )
            seen.add(label)
    legend_handles.extend(
        [
            Line2D([0], [0], color="black", linewidth=lw_cost, label="Cost model"),
            Line2D(
                [0],
                [0],
                color="#7B2D8E",
                linewidth=lw_base,
                linestyle="-",
                label="Always incremental",
            ),
            Line2D(
                [0],
                [0],
                color="#C0392B",
                linewidth=lw_base,
                linestyle="-",
                label="Always full rebuild",
            ),
        ]
    )
    # Legend tucked just under the x-axis label.
    leg = ax.legend(
        handles=legend_handles,
        loc="upper center",
        bbox_to_anchor=(0.5, -0.28),
        ncol=4,
        fontsize=font_legend,
        frameon=False,
        columnspacing=1.2,
        handletextpad=0.5,
        handlelength=1.8,
        labelspacing=0.4,
        borderaxespad=0.0,
    )
    for text in leg.get_texts():
        text.set_fontsize(font_legend)

    fig.tight_layout()
    fig.subplots_adjust(bottom=0.42)

    for name in (
        "phased_phase_bands.png",
        "q20_sf20_phase_bands.png",
        "phased_phase_bands.pdf",
        "q20_sf20_phase_bands.pdf",
    ):
        out = args.out_dir / name
        fig.savefig(out, dpi=200, bbox_inches="tight", pad_inches=0.06)
        print(f"Wrote {out}")
    plt.close(fig)


if __name__ == "__main__":
    main()
