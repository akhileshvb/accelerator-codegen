#!/usr/bin/env python3
"""Plot SF1 Postgres matview-vs-aux write-mix speedups (grouped bars, log y)."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
DEFAULT_CSV = ROOT / "results_pg_sf1" / "matview_vs_aux_write_mix.csv"
DEFAULT_OUT = ROOT / "results_pg_sf1" / "matview_vs_aux_write_mix_speedup.png"

WRITE_PCTS = [0, 1, 10, 50]
# Match the reference TPC-H grouped-bar palette (blue / gold / green / teal).
COLORS = ["#4C72B0", "#DDB753", "#55A868", "#2A9D8F"]
LABELS = ["0% writes (RO)", "1% writes", "10% writes", "50% writes"]


def fmt_label(value: float) -> str:
    if value <= 0:
        return "—"
    if value >= 1000:
        k = value / 1000.0
        text = f"{k:.1f}k" if k < 10 else f"{k:.0f}k"
        return text
    if value >= 100:
        return f"{value:.0f}"
    if value >= 10:
        return f"{value:.1f}"
    if value >= 1:
        return f"{value:.2f}"
    if value >= 0.1:
        return f"{value:.2f}"
    if value >= 0.01:
        return f"{value:.2f}"
    return "<0.01"


def _f(row: dict[str, str], key: str) -> float | None:
    raw = row.get(key)
    if raw in (None, "", "None"):
        return None
    return float(raw)


def load_speedup(csv_path: Path, column: str) -> dict[int, dict[int, float]]:
    by_q: dict[int, dict[int, float]] = {}
    with csv_path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            raw = row.get(column)
            if raw in (None, "", "None"):
                continue
            q = int(row["query"])
            pct = int(float(row["write_frequency_pct"]))
            by_q.setdefault(q, {})[pct] = float(raw)
    return by_q


def load_aux_vs_mv(csv_path: Path) -> dict[int, dict[int, float]]:
    return load_speedup(csv_path, "aux_speedup_vs_matview")


def load_aux_vs_ivm_with_matview_fallback(
    csv_path: Path,
) -> tuple[dict[int, dict[int, float]], set[int]]:
    """aux/ivm write-mix; unsupported queries use aux/matview. Adds 0% from reads."""
    by_q: dict[int, dict[int, float]] = {}
    fallback: set[int] = set()
    with csv_path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            q = int(row["query"])
            pct = int(float(row["write_frequency_pct"]))
            status = (row.get("ivm_status") or "").strip()
            use_fallback = status != "ok"
            if use_fallback:
                fallback.add(q)
                val = _f(row, "aux_speedup_vs_matview")
            else:
                val = _f(row, "aux_speedup_vs_ivm")
            if val is not None and val > 0:
                by_q.setdefault(q, {})[pct] = val

            # Read-only (0%): ratio of baseline read median to aux read median.
            if 0 not in by_q.get(q, {}):
                aux_r = _f(row, "aux_read_ms")
                if aux_r is None or aux_r <= 0:
                    continue
                if use_fallback:
                    base_r = _f(row, "matview_read_ms")
                else:
                    base_r = _f(row, "ivm_read_ms")
                if base_r is not None and base_r > 0:
                    by_q.setdefault(q, {})[0] = base_r / aux_r
    return by_q, fallback


def geomean(values: list[float]) -> float:
    positives = [v for v in values if v > 0]
    if not positives:
        return float("nan")
    return math.exp(sum(math.log(v) for v in positives) / len(positives))


def style() -> None:
    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 14,
            "axes.titlesize": 18,
            "axes.labelsize": 16,
            "xtick.labelsize": 14,
            "ytick.labelsize": 14,
            "legend.fontsize": 14,
            "figure.facecolor": "white",
            "axes.facecolor": "white",
            "axes.grid": True,
            "grid.alpha": 0.35,
            "grid.linestyle": "--",
            "axes.axisbelow": True,
            "savefig.dpi": 180,
        }
    )


def plot(
    by_q: dict[int, dict[int, float]],
    out_path: Path,
    title: str,
    *,
    ylabel: str = "Speedup aux / matview (×)",
    fallback_queries: set[int] | None = None,
    footnote: str = "",
) -> None:
    style()
    queries = sorted(by_q)
    if not queries:
        raise SystemExit("no rows to plot (empty speedup column?)")
    fallback_queries = fallback_queries or set()
    x = np.arange(len(queries))
    n = len(WRITE_PCTS)
    group = 0.84
    width = group / n

    fig, ax = plt.subplots(figsize=(18, 7.0), facecolor="white")
    # Axis floor keeps one pathological <<1× point (Q9@0%) from crushing the log scale.
    axis_floor = 0.05
    ymax = 1.0

    for j, pct in enumerate(WRITE_PCTS):
        offsets = x - group / 2 + width / 2 + j * width
        vals = [by_q[q].get(pct) for q in queries]
        heights = []
        for v in vals:
            if v is None or v <= 0:
                heights.append(0.0)
            else:
                heights.append(max(v, axis_floor))
                ymax = max(ymax, v)

        bars = ax.bar(
            offsets,
            heights,
            width=width * 0.92,
            color=COLORS[j],
            edgecolor="white",
            linewidth=0.45,
            label=LABELS[j],
            zorder=3,
        )
        for bar, v, h, q in zip(bars, vals, heights, queries):
            if v is None or v <= 0:
                continue
            if v < axis_floor or q in fallback_queries:
                bar.set_alpha(0.55 if q in fallback_queries else 0.45)
                bar.set_hatch("//")
                bar.set_edgecolor("#444444")
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                h,
                fmt_label(v),
                ha="center",
                va="bottom",
                rotation=90,
                fontsize=12,
                clip_on=False,
                zorder=4,
            )

    ax.set_yscale("log")
    ax.set_ylim(axis_floor, ymax * 3.2)
    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.05, zorder=1)
    ax.set_xticks(x)
    tick_labels = [
        f"Q{q}†" if q in fallback_queries else f"Q{q}" for q in queries
    ]
    ax.set_xticklabels(tick_labels)
    ax.set_ylabel(ylabel)
    ax.set_xlabel("Query")
    ax.set_title(title, pad=12)
    ax.grid(axis="y", linestyle="--", alpha=0.4, which="major", zorder=0)
    ax.grid(axis="x", linestyle=":", alpha=0.2, zorder=0)
    ax.legend(frameon=False, loc="upper left", ncol=4, fontsize=14, handlelength=1.4)
    for spine in ax.spines.values():
        spine.set_color("#333333")
        spine.set_linewidth(0.8)
    if footnote:
        fig.text(0.01, 0.01, footnote, fontsize=11, color="#444444", ha="left", va="bottom")

    geos = []
    for pct in WRITE_PCTS:
        vals = [by_q[q][pct] for q in queries if pct in by_q[q] and by_q[q][pct] > 0]
        geos.append(geomean(vals))
    fig.tight_layout(rect=(0, 0.03, 1, 1) if footnote else None)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_path}")
    print("geomean:", " · ".join(f"{p}%={g:.2f}×" for p, g in zip(WRITE_PCTS, geos)))
    if fallback_queries:
        print(
            "matview fallback queries:",
            ", ".join(f"Q{q}" for q in sorted(fallback_queries)),
        )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument(
        "--metric",
        choices=("aux_vs_matview", "aux_vs_ivm", "ivm_vs_matview"),
        default="aux_vs_matview",
        help="Which speedup column to plot (default: aux vs REFRESH matview).",
    )
    ap.add_argument("--title", default="")
    args = ap.parse_args()
    if not args.csv.is_file():
        raise SystemExit(f"missing csv: {args.csv}")

    if args.metric == "aux_vs_ivm":
        by_q, fallback = load_aux_vs_ivm_with_matview_fallback(args.csv)
        out = (
            args.out.with_name(args.out.stem.replace("matview", "ivm") + args.out.suffix)
            if "matview" in args.out.stem
            else args.out.with_name("matview_vs_aux_vs_ivm_write_mix_speedup.png")
        )
        title = args.title or (
            "TPC-H: Aux vs pg_ivm IMMV speedup under write mix "
            "(† = matview baseline; pg_ivm unsupported)"
        )
        plot(
            by_q,
            out,
            title,
            ylabel="Speedup aux / baseline (×)",
            fallback_queries=fallback,
            footnote="† Hatched: pg_ivm unsupported — baseline is REFRESH matview (aux/matview).",
        )
        return

    metric_cfg = {
        "aux_vs_matview": (
            "aux_speedup_vs_matview",
            "Speedup aux / matview (×)",
            "TPC-H: Aux vs matview speedup under write mix",
            args.out,
        ),
        "ivm_vs_matview": (
            "ivm_speedup_vs_matview",
            "Speedup pg_ivm IMMV / matview (×)",
            "TPC-H: pg_ivm IMMV vs matview speedup under write mix",
            args.out.with_name("ivm_vs_matview_write_mix_speedup.png"),
        ),
    }
    col, ylabel, default_title, out = metric_cfg[args.metric]
    title = args.title or default_title
    by_q = load_speedup(args.csv, col)
    if args.metric == "aux_vs_matview":
        with args.csv.open(newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                q = int(row["query"])
                if 0 in by_q.get(q, {}):
                    continue
                aux_r = _f(row, "aux_read_ms")
                mv_r = _f(row, "matview_read_ms")
                if aux_r and aux_r > 0 and mv_r and mv_r > 0:
                    by_q.setdefault(q, {})[0] = mv_r / aux_r
    plot(by_q, out, title, ylabel=ylabel)


if __name__ == "__main__":
    main()
