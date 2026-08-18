#!/usr/bin/env python3
"""Three-way comparison plots: us vs GenDB vs Bespoke (speedups + memory).

Plot 1 — TPC-H SF10, no aux, read-only: Us ST, Us MT (t=8), GenDB MT.
Plot 2 — TPC-H SF20 aux read-only: Us ST, GenDB MT (index), Bespoke ST.
Plot 3 — TPC-H SF20 total workload memory: Us vs GenDB vs Bespoke.
"""

from __future__ import annotations

import csv
import statistics
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
RESULTS = Path(__file__).resolve().parent / "results"
TPCH_RESULTS = ROOT / "benchmarks" / "tpch" / "results"
TPCH_AUX_QUERIES = (1, 3, 6, 9, 10, 11, 12, 14, 17, 18, 21)

SF20_AUX_HEADER = (
    "suite,write_mode,write_policy,locality,write_frequency_pct,write_interval_sec,"
    "measured_duration_sec,reader_qps,query_num,num_queries,writes_executed,"
    "duckdb_ref_query_median_us,base_kernel_median_us,base_write_median_us,"
    "accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,"
    "fallback_rate,total_duckdb_query_us,total_accel_query_us,total_base_write_us,"
    "total_accel_write_us,duckdb_workload_us,accel_workload_us,workload_speedup,"
    "cold_start_workload_speedup,query_only_speedup,blended_query_speedup,"
    "cold_start_accel_workload_us,setup_full_build_ms,aux_state_bytes,valid_partitions,"
    "total_slots"
).split(",")

# External baselines (GenDB / Bespoke; not measured in this repo).
GENDDB_SF10_MT_NO_INDEX = 2.04
GENDDB_AUX_MT_NO_INDEX = 2.09
GENDDB_AUX_MT_INDEX = 2.15
BESPOKE_AUX_ST = 16.4

# Total persistent footprint (GB). Us is measured (base DuckDB SF20 + all
# 11 aux states). Prefer packed remeasure logs when present; speedups still
# come from the 120s read-only rows. GenDB and Bespoke are the encoded
# columns/indexes actually touched by those same 11 queries.
SF20_DB_PATH = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf20.duckdb"
SF20_PACKED_MEMORY_DIR = TPCH_RESULTS / "sf20_memory_packed"
BESPOKE_MEMORY_CSV = RESULTS / "bespoke_tpch_sf20_memory.csv"
GENDDB_MEMORY_CSV = RESULTS / "gendb_tpch_sf20_memory.csv"

# Columns read by TPC-H Q1,3,6,9,10,11,12,14,17,18,21 in the Bespoke artifact.
BESPOKE_AUX_COLUMNS = frozenset(
    {
        "lineitem.orderkey",
        "lineitem.partkey",
        "lineitem.suppkey",
        "lineitem.shipdate",
        "lineitem.extendedprice",
        "lineitem.discount",
        "lineitem.tax",
        "lineitem.quantity",
        "lineitem.returnflag",
        "lineitem.linestatus",
        "lineitem.shipmode",
        "lineitem.supp_nationkey",
        "lineitem.commit_receipt",
        "lineitem.discounted_price",
        "lineitem.returnflag_linestatus",
        "lineitem.shards",
        "orders.orderkey",
        "orders.custkey",
        "orders.orderdate",
        "orders.shippriority",
        "orders.totalprice",
        "orders.orderstatus",
        "orders.orderpriority",
        "orders.orderkey_to_row",
        "orders.lineitem_ranges",
        "customer.custkey",
        "customer.nationkey",
        "customer.acctbal",
        "customer.mktsegment",
        "customer.name",
        "customer.address",
        "customer.phone",
        "customer.comment",
        "part.partkey",
        "part.brand",
        "part.container",
        "part.name",
        "part.type",
        "supplier.suppkey",
        "supplier.nationkey",
        "supplier.name",
        "partsupp.partkey",
        "partsupp.suppkey",
        "partsupp.supplycost",
        "partsupp.supplycost_availqty",
        "nation",
    }
)

US_COLOR = "#4C72B0"
GENDDB_COLOR = "#DD8452"
GENDDB_INDEX_COLOR = "#C44E52"
BESPOKE_COLOR = "#55A868"


def fmt_speedup(value: float) -> str:
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def median_sf10_non_aux_st() -> float:
    values: list[float] = []
    with (TPCH_RESULTS / "tpch_speedup_all_queries.csv").open(newline="") as handle:
        for row in csv.DictReader(handle):
            values.append(float(row["sf10_speedup"]))
    return statistics.median(values)


def median_sf10_non_aux_mt8() -> float:
    values: list[float] = []
    with (TPCH_RESULTS / "sf10_parallel_speedup_22q.csv").open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["query"].startswith("overall"):
                continue
            values.append(float(row["t=8"]))
    return statistics.median(values)


def sf20_aux_readonly_row(query: int) -> dict[str, str]:
    log = TPCH_RESULTS / f"tpch_aux_sf20_q{query}.log"
    if not log.is_file():
        raise SystemExit(f"missing {log}")
    for line in log.read_text().splitlines():
        if not line.startswith("lineitem_only,") or ",0,0,120," not in line:
            continue
        return dict(zip(SF20_AUX_HEADER, line.split(",")[: len(SF20_AUX_HEADER)]))
    raise SystemExit(f"no read-only row in {log}")


def median_tpch_aux_read_st() -> float:
    values: list[float] = []
    for query in TPCH_AUX_QUERIES:
        row = sf20_aux_readonly_row(query)
        speedup = float(row["cold_start_workload_speedup"])
        if speedup <= 0:
            raise SystemExit(f"Q{query} has non-positive cold_start_workload_speedup")
        values.append(speedup)
    return statistics.median(values)


def packed_aux_state_bytes(query: int) -> int | None:
    log = SF20_PACKED_MEMORY_DIR / f"tpch_aux_sf20_q{query}.log"
    if not log.is_file():
        return None
    for line in log.read_text().splitlines():
        if not line.startswith("lineitem_only,"):
            continue
        parts = line.split(",")
        if len(parts) > 31 and parts[4] == "0" and parts[5] == "0":
            return int(float(parts[31]))
    return None


def us_aux_memory_mib_sf20(query: int) -> float:
    packed = packed_aux_state_bytes(query)
    if packed is not None:
        return packed / (1024 * 1024)
    row = sf20_aux_readonly_row(query)
    return int(float(row["aux_state_bytes"])) / (1024 * 1024)


def fmt_gb(value: float) -> str:
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return f"{text} GB"


def sf20_us_total_gb() -> float:
    if not SF20_DB_PATH.is_file():
        raise SystemExit(f"missing SF20 DuckDB: {SF20_DB_PATH}")
    base = SF20_DB_PATH.stat().st_size / (1024 ** 3)
    aux = sum(us_aux_memory_mib_sf20(q) for q in TPCH_AUX_QUERIES) / 1024.0
    return round(base + aux, 1)


def sf20_bespoke_aux_gb() -> float:
    if not BESPOKE_MEMORY_CSV.is_file():
        raise SystemExit(f"missing {BESPOKE_MEMORY_CSV}")
    total = 0
    found: set[str] = set()
    with BESPOKE_MEMORY_CSV.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["section"] != "storage" or row["name"] not in BESPOKE_AUX_COLUMNS:
                continue
            found.add(row["name"])
            total += int(row["bytes"])
    missing = BESPOKE_AUX_COLUMNS - found
    if missing:
        raise SystemExit(f"bespoke memory CSV missing columns: {sorted(missing)}")
    return round(total / (1024 ** 3), 2)


def sf20_gendb_aux_gb() -> float:
    if not GENDDB_MEMORY_CSV.is_file():
        raise SystemExit(f"missing {GENDDB_MEMORY_CSV}")
    total = 0
    found = 0
    with GENDDB_MEMORY_CSV.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["section"] != "storage" or row["used_by_aux_11"] != "1":
                continue
            found += 1
            total += int(row["bytes"])
    if found == 0:
        raise SystemExit(f"no aux-11 rows in {GENDDB_MEMORY_CSV}")
    return round(total / (1024 ** 3), 2)


def write_summary_csv(rows: list[dict[str, str | float]]) -> None:
    RESULTS.mkdir(parents=True, exist_ok=True)
    path = RESULTS / "gendb_bespoke_comparison_summary.csv"
    fields = ["plot", "series", "metric", "value", "unit", "notes"]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {path}")


def plot_speedup_bars(
    *,
    title: str,
    subtitle: str,
    labels: list[str],
    values: list[float],
    colors: list[str],
    out_path: Path,
    log_scale: bool = False,
) -> None:
    fig, ax = plt.subplots(figsize=(8.0, 5.5), facecolor="white")
    x = np.arange(len(labels))
    bars = ax.bar(
        x,
        values,
        color=colors,
        edgecolor="white",
        linewidth=0.8,
        width=0.62,
        zorder=3,
    )
    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.0, zorder=1)
    if log_scale:
        ax.set_yscale("log")
        ax.set_ylabel("Median speedup vs DuckDB (×, log scale)")
    else:
        ax.set_ylabel("Median speedup vs DuckDB (×)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ymax = max(values)
    ymin = min(values)
    if log_scale:
        ax.set_ylim(max(ymin / 2.0, 0.5), ymax * 2.5)
    else:
        pad = max(0.15, ymax * 0.12)
        ax.set_ylim(0, ymax + pad)
    for bar, value in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height(),
            fmt_speedup(value),
            ha="center",
            va="bottom",
            fontsize=11,
            fontweight="medium",
            clip_on=False,
        )
    ax.grid(axis="y", linestyle=":", alpha=0.45, zorder=0)
    ax.set_title(f"{title}\n{subtitle}", fontsize=12, pad=12)
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_memory_totals(
    bars: list[tuple[str, float, str]],
    out_path: Path,
) -> None:
    labels = [item[0] for item in bars]
    values = [item[1] for item in bars]
    colors = [item[2] for item in bars]
    x = np.arange(len(labels))

    fig, ax = plt.subplots(figsize=(8.0, 5.5), facecolor="white")
    rects = ax.bar(
        x,
        values,
        color=colors,
        edgecolor="white",
        linewidth=0.8,
        width=0.62,
        zorder=3,
    )
    for rect, value in zip(rects, values):
        ax.text(
            rect.get_x() + rect.get_width() / 2,
            rect.get_height(),
            fmt_gb(value),
            ha="center",
            va="bottom",
            fontsize=11,
            fontweight="medium",
            clip_on=False,
        )
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylabel("Total footprint (GB)")
    ax.set_ylim(0, max(values) * 1.16)
    ax.grid(axis="y", linestyle=":", alpha=0.45, zorder=0)
    ax.set_title(
        "TPC-H SF20 workload memory (11 aux queries)\n"
        f"Us {fmt_gb(values[0])}  ·  GenDB {fmt_gb(values[1])}  ·  "
        f"Bespoke {fmt_gb(values[2])}",
        fontsize=12,
        pad=12,
    )
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_path}")


def main() -> None:
    us_st = median_sf10_non_aux_st()
    us_mt = median_sf10_non_aux_mt8()
    us_aux = median_tpch_aux_read_st()
    us_total_gb = sf20_us_total_gb()
    gendb_total_gb = sf20_gendb_aux_gb()
    bespoke_total_gb = sf20_bespoke_aux_gb()

    summary: list[dict[str, str | float]] = [
        {
            "plot": "non_aux_sf10",
            "series": "accelerator_st",
            "metric": "median_speedup",
            "value": round(us_st, 4),
            "unit": "x",
            "notes": "22 TPC-H queries; tpch_speedup_all_queries.csv sf10_speedup",
        },
        {
            "plot": "non_aux_sf10",
            "series": "accelerator_mt8",
            "metric": "median_speedup",
            "value": round(us_mt, 4),
            "unit": "x",
            "notes": "22 TPC-H queries; sf10_parallel_speedup_22q.csv t=8",
        },
        {
            "plot": "non_aux_sf10",
            "series": "gendb_mt_no_index",
            "metric": "median_speedup",
            "value": GENDDB_SF10_MT_NO_INDEX,
            "unit": "x",
            "notes": "external baseline",
        },
        {
            "plot": "aux_readonly",
            "series": "accelerator_st",
            "metric": "median_speedup",
            "value": round(us_aux, 4),
            "unit": "x",
            "notes": "TPC-H SF20 aux Q1,3,6,9,10,11,12,14,17,18,21; cold_start_workload_speedup at interval=0",
        },
        {
            "plot": "aux_readonly",
            "series": "gendb_mt_index",
            "metric": "median_speedup",
            "value": GENDDB_AUX_MT_INDEX,
            "unit": "x",
            "notes": "external baseline",
        },
        {
            "plot": "aux_readonly",
            "series": "bespoke_st",
            "metric": "median_speedup",
            "value": BESPOKE_AUX_ST,
            "unit": "x",
            "notes": "external baseline",
        },
    ]
    summary.extend(
        [
            {
                "plot": "memory_sf20",
                "series": "accelerator_total",
                "metric": "workload_footprint",
                "value": us_total_gb,
                "unit": "gb",
                "notes": "base DuckDB SF20 + all 11 aux states",
            },
            {
                "plot": "memory_sf20",
                "series": "gendb_total",
                "metric": "workload_footprint",
                "value": gendb_total_gb,
                "unit": "gb",
                "notes": "measured on-disk files used by Q1,3,6,9,10,11,12,14,17,18,21",
            },
            {
                "plot": "memory_sf20",
                "series": "bespoke_total",
                "metric": "workload_footprint",
                "value": bespoke_total_gb,
                "unit": "gb",
                "notes": "encoded columns used by Q1,3,6,9,10,11,12,14,17,18,21",
            },
        ]
    )
    write_summary_csv(summary)

    plot_speedup_bars(
        title="TPC-H SF10 read-only (no auxiliary state)",
        subtitle=(
            f"Median over 22 queries — Us ST {fmt_speedup(us_st)} · "
            f"Us MT@8 {fmt_speedup(us_mt)} · GenDB MT {fmt_speedup(GENDDB_SF10_MT_NO_INDEX)}"
        ),
        labels=["Us ST", "Us MT (t=8)", "GenDB MT\n(no index)"],
        values=[us_st, us_mt, GENDDB_SF10_MT_NO_INDEX],
        colors=[US_COLOR, US_COLOR, GENDDB_COLOR],
        out_path=RESULTS / "non_aux_sf10_median_speedup.png",
    )

    plot_speedup_bars(
        title="TPC-H SF20 read-only (auxiliary state, cold start)",
        subtitle=(
            f"Median over 11 aux queries — Us ST {fmt_speedup(us_aux)} · "
            f"GenDB index {fmt_speedup(GENDDB_AUX_MT_INDEX)} · "
            f"Bespoke ST {fmt_speedup(BESPOKE_AUX_ST)}"
        ),
        labels=["Us ST", "GenDB MT\n(index)", "Bespoke ST"],
        values=[us_aux, GENDDB_AUX_MT_INDEX, BESPOKE_AUX_ST],
        colors=[US_COLOR, GENDDB_INDEX_COLOR, BESPOKE_COLOR],
        out_path=RESULTS / "aux_readonly_median_speedup.png",
        log_scale=True,
    )

    memory_bars = [
        ("Us", us_total_gb, US_COLOR),
        ("GenDB", gendb_total_gb, GENDDB_INDEX_COLOR),
        ("Bespoke OLAP", bespoke_total_gb, BESPOKE_COLOR),
    ]
    plot_memory_totals(memory_bars, RESULTS / "aux_memory_total_workload_sf20.png")
    plot_memory_totals(memory_bars, RESULTS / "aux_memory_us_vs_bespoke_sf20.png")


if __name__ == "__main__":
    main()
