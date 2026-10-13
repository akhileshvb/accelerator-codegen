#!/usr/bin/env python3
"""TPC-H SF20 read-only speedup: per-query bars vs DuckDB (top) and Postgres (bottom).

Top panel (vs DuckDB, log scale):
  - JetStream stateless   benchmarks/tpch/results/stateless_duckdb_sf20_summary.csv
  - GenDB (MT)            external baseline, transcribed from the earlier figure
  - Bespoke OLAP (ST)     external baseline, transcribed from the earlier figure
  - JetStream stateful    aux_compare read JSONs (median over reps of per-rep geomean)
Bottom panel (vs Postgres): JetStream stateless (same thread count as the top
panel) and JetStream stateful, each from its own CSV.

Usage: plot_readonly_speedup.py PG_STATEFUL_CSV PG_STATELESS_CSV [OUT_PREFIX]
"""

from __future__ import annotations

import csv
import glob
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
STATELESS_CSV = ROOT / "benchmarks/tpch/results/stateless_duckdb_sf20_summary.csv"
STATELESS_COL = "par_speedup_32t"  # or st_speedup / par_speedup_16t
STATELESS_LABEL = "JetStream (stateless, 32T)"
STATELESS_PG_THREADS = "32"
STATEFUL_DIR = ROOT / "benchmarks/compare/results/rf_interval_multi_sf20_20261008_112014/read"
QUERIES = list(range(1, 23))

# External baselines (not measured in this repo); values as printed on the
# earlier read-only figure. GenDB Q17 was printed as "1.4k".
GENDB_MT = [18.6, 11.7, 23.0, 77.5, 28.7, 8.5, 27.6, 36.8, 30.2, 40.8, 4.7,
            50.2, 17.4, 34.6, 47.1, 15.4, 1400.0, 51.5, 54.6, 62.0, 70.4, 25.0]
BESPOKE_ST = [26.0, 19.0, 4.6, 28.0, 15.0, 4.1, 2.8, 4.1, 9.2, 12.0, 8.6,
              11.0, 6.2, 23.0, 9.4, 5.2, 12.0, 37.0, 66.0, 7.6, 14.0, 100.0]

STATELESS_COLOR = "#4C72B0"
GENDB_COLOR = "#D4A020"
BESPOKE_COLOR = "#55A868"
STATEFUL_COLOR = "#2A9D8F"
VERIFIED_COLOR = "#3B1F6B"
PG_COLOR = "#C44E64"


def geomean(values: list[float | None]) -> float:
    present = [v for v in values if v is not None]
    return math.exp(sum(math.log(v) for v in present) / len(present))


def geomean_label(values: list[float | None], digits: int) -> str:
    """Geomean for a legend entry; notes how many queries it covers if any are missing."""
    n = sum(v is not None for v in values)
    suffix = "" if n == len(values) else f", {n} queries"
    return f"{geomean(values):.{digits}f}×{suffix}"


def fmt(value: float) -> str:
    if value >= 10000:
        return f"{value / 1000:.0f}k"
    if value >= 1000:
        return f"{value / 1000:.1f}k"
    if value >= 100:
        return f"{value:.0f}"
    return f"{value:.1f}"


def load_stateless() -> list[float]:
    by_query = {}
    with STATELESS_CSV.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["query"].startswith("Q"):
                by_query[int(row["query"][1:])] = float(row[STATELESS_COL])
    return [by_query[q] for q in QUERIES]


def load_stateless_postgres(path: Path) -> list[float]:
    by_query = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["threads"] == STATELESS_PG_THREADS:
                by_query[int(row["query"].lstrip("Q"))] = float(row["speedup"])
    return [by_query[q] for q in QUERIES]


def load_stateful(store: str) -> list[float | None]:
    """None for a query with no results yet (e.g. a run still in progress)."""
    out = []
    for q in QUERIES:
        reps = [json.load(open(f)) for f in sorted(glob.glob(str(STATEFUL_DIR / f"read_q{q}_{store}_r*.json")))]
        if not reps:
            print(f"warning: no {store} read results for Q{q} in {STATEFUL_DIR}; leaving it out")
            out.append(None)
            continue
        if any(r["mismatches"] for r in reps):
            raise SystemExit(f"Q{q} {store} run has result mismatches")
        out.append(statistics.median(r["speedup_geomean"] for r in reps))
    return out


def load_postgres(path: Path) -> tuple[list[float], set[int]]:
    """Median over reps of the per-rep geomean speedup; also queries with mismatches."""
    per_rep: dict[int, dict[int, list[float]]] = defaultdict(lambda: defaultdict(list))
    mismatched: set[int] = set()
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            q = int(row["query"])
            per_rep[q][int(row["rep"])].append(float(row["speedup"]))
            if row["result_match"] != "True":
                mismatched.add(q)
    values = [statistics.median(geomean(v) for v in per_rep[q].values()) for q in QUERIES]
    return values, mismatched


LABEL_SIZE = 19
PG_LABEL_SIZE = 26
TICK_SIZE = 28
AXIS_LABEL_SIZE = 31


def label_bars(ax, bars, values, size: float = LABEL_SIZE) -> list:
    return [ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 1.15, fmt(value),
                    ha="center", va="bottom", rotation=90, fontsize=size, color="#333333")
            if value is not None else None
            for bar, value in zip(bars, values)]


def place_labels(fig, ax, groups: list[list[tuple]]) -> int:
    """Keep each label on its own bar. Where a label above its bar would collide
    with its left neighbour's, move one of the two inside the top of its bar
    (white text) when that bar is tall enough. Returns collisions left over."""
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()

    def extent(text):
        return text.get_window_extent(renderer)

    def bar_px(bar):
        return ax.transData.transform((0, bar.get_height()))[1] - ax.transData.transform((0, ax.get_ylim()[0]))[1]

    def move_inside(text, bar) -> bool:
        if bar_px(bar) < extent(text).height + 8:
            return False
        text.set_y(bar.get_height() / 1.12)
        text.set_va("top")
        text.set_color("white")
        return True

    def collide(a, b) -> bool:
        # rotated labels: the right edge of each box is the font's descent, empty for digits, so it may overlap
        ea, eb = extent(a), extent(b)
        slack = 0.15 * a.get_fontsize() * fig.dpi / 72
        return ea.x1 > eb.x0 + slack and eb.x1 > ea.x0 + slack and ea.y1 > eb.y0 and eb.y1 > ea.y0

    left = 0
    for group in groups:
        group = [(t, b) for t, b in group if t is not None]
        for (prev, prev_bar), (cur, cur_bar) in zip(group, group[1:]):
            if not collide(prev, cur):
                continue
            # Prefer moving the taller bar's label inside; it has the room.
            order = [(cur, cur_bar), (prev, prev_bar)]
            if prev_bar.get_height() > cur_bar.get_height():
                order.reverse()
            if not any(move_inside(t, b) for t, b in order):
                left += 1
    return left


def style_axis(ax, n: int) -> None:
    ax.set_yscale("log")
    ax.axhline(1.0, color="#888888", linestyle="--", linewidth=0.8, zorder=1)
    ax.grid(axis="y", linestyle=":", alpha=0.5, zorder=0)
    ax.set_facecolor("#FAFAFA")
    ax.set_xlim(-0.55, n - 0.45)
    ax.tick_params(axis="y", labelsize=TICK_SIZE)
    ax.tick_params(axis="x", labelsize=TICK_SIZE)


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    pg_csv = Path(sys.argv[1])
    pg_stateless_csv = Path(sys.argv[2])
    out_prefix = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / "benchmarks/compare/results/readonly_speedup_sf20"

    stateless = load_stateless()
    stateful = load_stateful("cpp")
    verified = load_stateful("verus")
    pg, pg_mismatch = load_postgres(pg_csv)
    pg_stateless = load_stateless_postgres(pg_stateless_csv)

    # One legend for both panels; series in both list both geomeans.
    top = [
        (f"{STATELESS_LABEL} (DuckDB: {geomean(stateless):.2f}×, Postgres: {geomean(pg_stateless):.2f}×)",
         stateless, STATELESS_COLOR),
        (f"GenDB (MT) (DuckDB: {geomean(GENDB_MT):.2f}×)", GENDB_MT, GENDB_COLOR),
        (f"Bespoke OLAP (ST) (DuckDB: {geomean(BESPOKE_ST):.2f}×)", BESPOKE_ST, BESPOKE_COLOR),
        (f"JetStream (stateful) (DuckDB: {geomean_label(stateful, 0)}, Postgres: {geomean(pg):.0f}×)",
         stateful, STATEFUL_COLOR),
        (f"JetStream (stateful, verified) (DuckDB: {geomean_label(verified, 0)})", verified, VERIFIED_COLOR),
    ]
    bottom = [
        ("_stateless", pg_stateless, STATELESS_COLOR),
        ("_stateful", pg, STATEFUL_COLOR),
    ]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(30, 14), sharex=True, facecolor="white",
                                   gridspec_kw={"height_ratios": [1.3, 1]})
    x = np.arange(len(QUERIES))

    def grouped(ax, series, size):
        width = 0.94 / len(series)
        labels = []
        for i, (label, values, color) in enumerate(series):
            heights = [np.nan if v is None else v for v in values]
            bars = ax.bar(x + (i - (len(series) - 1) / 2) * width, heights, width * 0.94,
                          color=color, label=label, zorder=3)
            labels.append(list(zip(label_bars(ax, bars, values, size), bars)))
        return [list(group) for group in zip(*labels)]

    top_groups = grouped(ax1, top, LABEL_SIZE)
    style_axis(ax1, len(QUERIES))
    present = [v for _, vs, _ in top for v in vs if v is not None]
    ax1.set_ylim(min(0.8, min(present) * 0.7), max(present) * 25)
    ax1.set_ylabel("Speedup vs DuckDB", fontsize=AXIS_LABEL_SIZE)
    ax1.set_title("TPC-H (SF=20): Read-only Speedup", fontsize=33)

    bottom_groups = grouped(ax2, bottom, PG_LABEL_SIZE)
    style_axis(ax2, len(QUERIES))
    ax2.set_ylim(0.8, max(max(v) for _, v, _ in bottom) * 150)
    ax2.set_ylabel("Speedup vs Postgres", fontsize=AXIS_LABEL_SIZE)
    ax2.set_xticks(x)
    ax2.set_xticklabels([f"Q{q}" for q in QUERIES])
    ax2.set_xlabel("Query", fontsize=AXIS_LABEL_SIZE)

    # two rows of three; the two longest entries share a column so the legend fits the width
    h = ax1.containers
    fig.legend(handles=[h[0], h[3], h[1], h[2], h[4]], loc="lower center", ncol=3, frameon=False, fontsize=27,
               bbox_to_anchor=(0.5, -0.01), handlelength=1.2, handletextpad=0.4, columnspacing=1.2)
    fig.tight_layout(rect=(0, 0.12, 1, 1))
    left = place_labels(fig, ax1, top_groups) + place_labels(fig, ax2, bottom_groups)
    if left:
        print(f"warning: {left} label collisions could not be resolved")
    out_prefix.parent.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(out_prefix.with_suffix(f".{ext}"), dpi=200, bbox_inches="tight", facecolor="white")
        print(f"wrote {out_prefix.with_suffix('.' + ext)}")

    with out_prefix.with_suffix(".csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["query", "stateless_vs_duckdb", "gendb_mt", "bespoke_st",
                         "stateful_vs_duckdb", "verified_vs_duckdb", "stateless_vs_postgres", "stateful_vs_postgres", "postgres_result_match"])
        for i, q in enumerate(QUERIES):
            writer.writerow([f"Q{q}", f"{stateless[i]:.3f}", GENDB_MT[i], BESPOKE_ST[i],
                             f"{stateful[i]:.1f}", "" if verified[i] is None else f"{verified[i]:.1f}", f"{pg_stateless[i]:.3f}", f"{pg[i]:.1f}", q not in pg_mismatch])
        writer.writerow(["geomean", f"{geomean(stateless):.3f}", f"{geomean(GENDB_MT):.2f}",
                         f"{geomean(BESPOKE_ST):.2f}", f"{geomean(stateful):.1f}", f"{geomean(verified):.1f}", f"{geomean(pg_stateless):.3f}", f"{geomean(pg):.1f}", ""])
    print(f"wrote {out_prefix.with_suffix('.csv')}")


if __name__ == "__main__":
    main()
