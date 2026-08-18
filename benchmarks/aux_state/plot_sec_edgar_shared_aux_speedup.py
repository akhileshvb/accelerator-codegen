#!/usr/bin/env python3
"""Plot SEC-EDGAR SF3 shared-aux speedup for DuckDB and Postgres."""

from __future__ import annotations

import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

RESULTS = Path(__file__).resolve().parent / "results"
QUERIES = (1, 2, 3, 4, 6, 24)
INTERVALS = (0.0, 0.5, 1.0, 2.0)
WRITE_INTERVALS = (0.5, 1.0, 2.0)
COLORS = {
    1: "#4C72B0",
    2: "#DD8452",
    3: "#55A868",
    4: "#C44E52",
    6: "#8172B2",
    24: "#937860",
}
MARKERS = {1: "o", 2: "s", 3: "^", 4: "D", 6: "v", 24: "P"}
DUCKDB_BAR = "#4C72B0"
POSTGRES_BAR = "#DD8452"

# Published GenDB multithreaded read latencies (SEC-EDGAR SF3).
GENDDB_MT_MS = {
    1: 12.7,
    2: 26.1,
    3: 27.3,
    4: 99.2,
    6: 100.0,
    24: 62.6,
}


def fmt_speedup(value: float) -> str:
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    if value >= 1:
        return f"{value:.2f}×"
    return f"{value:.3f}×"


def geomean(values: list[float]) -> float:
    positives = [v for v in values if v > 0]
    return math.exp(sum(math.log(v) for v in positives) / len(positives))


def load_summary(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return [
            row
            for row in csv.DictReader(handle)
            if row.get("workload") == "sec_edgar"
        ]


def postgres_with_q1(
    summary_path: Path, combined_path: Path
) -> list[dict[str, str]]:
    rows = load_summary(summary_path)
    have_q1 = any(int(row["query"]) == 1 for row in rows)
    if have_q1 or not combined_path.is_file():
        return rows

    by_interval: dict[float, dict[str, str]] = {}
    with combined_path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("workload") != "sec_edgar" or int(row["query"]) != 1:
                continue
            if row.get("mode") != "shared":
                continue
            interval = float(row["write_interval_sec"])
            by_interval[interval] = {
                "engine": "postgres",
                "workload": "sec_edgar",
                "query": "1",
                "write_interval_sec": row["write_interval_sec"],
                "read_speedup": row.get("read_speedup_p50") or "",
                "workload_speedup": row["workload_speedup_vs_base"],
            }
    rows.extend(by_interval[i] for i in INTERVALS if i in by_interval)
    return rows


def index_rows(
    rows: list[dict[str, str]],
) -> dict[tuple[int, float], dict[str, float]]:
    out: dict[tuple[int, float], dict[str, float]] = {}
    for row in rows:
        query = int(row["query"])
        interval = float(row["write_interval_sec"])
        read = row.get("read_speedup") or ""
        work = row.get("workload_speedup") or ""
        if not read or not work:
            continue
        out[(query, interval)] = {
            "read": float(read),
            "workload": float(work),
        }
    return out


def _draw_readonly_bars(
    ax,
    queries: list[int],
    duckdb_vals: list[float],
    postgres_vals: list[float],
    ylabel: str,
    subtitle: str,
    ymin_floor: float,
) -> None:
    labels = [f"Q{q}" for q in queries]
    x = np.arange(len(queries))
    width = 0.38
    duck_bars = ax.bar(
        x - width / 2,
        duckdb_vals,
        width=width,
        color=DUCKDB_BAR,
        edgecolor="white",
        linewidth=0.6,
        label="DuckDB",
        zorder=3,
    )
    pg_bars = ax.bar(
        x + width / 2,
        postgres_vals,
        width=width,
        color=POSTGRES_BAR,
        edgecolor="white",
        linewidth=0.6,
        label="Postgres",
        zorder=3,
    )
    ax.axhline(1.0, color="#666666", linestyle="--", linewidth=1.0, zorder=1)
    ax.set_yscale("log")
    for bars in (duck_bars, pg_bars):
        for bar in bars:
            value = bar.get_height()
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                value,
                fmt_speedup(value),
                ha="center",
                va="bottom",
                fontsize=9,
                fontweight="medium",
                clip_on=False,
            )
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylabel(ylabel, fontsize=11)
    ax.set_xlabel("Query", fontsize=11)
    ymax = max(max(duckdb_vals), max(postgres_vals))
    ymin = min(min(duckdb_vals), min(postgres_vals))
    ax.set_ylim(max(ymin / 2.5, ymin_floor), ymax * 2.6)
    ax.grid(axis="y", which="both", linestyle=":", alpha=0.45, zorder=0)
    ax.legend(loc="upper left", framealpha=0.92, fontsize=10)
    ax.set_title(subtitle, fontsize=11, pad=10)


def plot_readonly_combined(
    duckdb: dict[tuple[int, float], dict[str, float]],
    postgres: dict[tuple[int, float], dict[str, float]],
    out_path: Path,
    title: str = "SEC-EDGAR SF3 aux speedup (read-only)",
    subtitle_prefix: str = "shared aux vs host base",
    gendb_ms: dict[int, float] | None = None,
) -> None:
    queries = [q for q in QUERIES if (q, 0.0) in duckdb and (q, 0.0) in postgres]
    duckdb_vals = [duckdb[(q, 0.0)]["read"] for q in queries]
    postgres_vals = [postgres[(q, 0.0)]["read"] for q in queries]
    host_subtitle = (
        f"{subtitle_prefix}  ·  DuckDB geomean {fmt_speedup(geomean(duckdb_vals))}"
        f"  ·  Postgres geomean {fmt_speedup(geomean(postgres_vals))}"
    )

    vs_gendb = gendb_ms is not None and all(
        "aux_read_us" in duckdb[(q, 0.0)] and "aux_read_us" in postgres[(q, 0.0)]
        and q in gendb_ms
        for q in queries
    )
    if vs_gendb:
        fig, axes = plt.subplots(1, 2, figsize=(14.8, 6.0), facecolor="white")
        ax_host, ax_gendb = axes
    else:
        fig, ax_host = plt.subplots(figsize=(10.2, 6.0), facecolor="white")
        ax_gendb = None

    _draw_readonly_bars(
        ax_host,
        queries,
        duckdb_vals,
        postgres_vals,
        "Read speedup vs base (×)",
        host_subtitle if vs_gendb else f"{title}\n{host_subtitle}",
        ymin_floor=0.3,
    )

    if ax_gendb is not None and gendb_ms is not None:
        duckdb_vs_g = [
            gendb_ms[q] * 1000.0 / duckdb[(q, 0.0)]["aux_read_us"] for q in queries
        ]
        postgres_vs_g = [
            gendb_ms[q] * 1000.0 / postgres[(q, 0.0)]["aux_read_us"] for q in queries
        ]
        _draw_readonly_bars(
            ax_gendb,
            queries,
            duckdb_vs_g,
            postgres_vs_g,
            "Read speedup vs GenDB MT (×)",
            "aux_parallel vs published GenDB MT  ·  "
            f"DuckDB geomean {fmt_speedup(geomean(duckdb_vs_g))}"
            f"  ·  Postgres geomean {fmt_speedup(geomean(postgres_vs_g))}",
            ymin_floor=0.08,
        )
        fig.suptitle(title, fontsize=13, y=1.02)

    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_write_interval(
    indexed: dict[tuple[int, float], dict[str, float]],
    backend: str,
    out_path: Path,
    subtitle: str,
) -> None:
    fig, ax = plt.subplots(figsize=(10.5, 6.4), facecolor="white")
    xs = list(WRITE_INTERVALS)
    all_values: list[float] = []
    ro_by_query: list[tuple[int, float]] = []
    for query in QUERIES:
        ro = indexed.get((query, 0.0))
        if ro is not None:
            ro_by_query.append((query, ro["read"]))
        ys = []
        plot_xs = []
        for interval in WRITE_INTERVALS:
            point = indexed.get((query, interval))
            if point is None:
                continue
            plot_xs.append(interval)
            ys.append(point["workload"])
            all_values.append(point["workload"])
        if not ys:
            continue
        ax.plot(
            plot_xs,
            ys,
            label=f"Q{query}",
            color=COLORS[query],
            marker=MARKERS[query],
            linewidth=2.1,
            markersize=8,
            zorder=3,
        )

    ro_by_query.sort(key=lambda item: item[1])
    if ro_by_query:
        logs = [math.log10(v) for _, v in ro_by_query]
        lo, hi = min(logs), max(logs)
        min_span = 0.16 * (len(logs) - 1) if len(logs) > 1 else 0.0
        span = max(hi - lo, min_span)
        start = (lo + hi) / 2 - span / 2
        step = span / (len(logs) - 1) if len(logs) > 1 else 0.0
        for i, (query, ro_speedup) in enumerate(ro_by_query):
            ax.axhline(
                ro_speedup,
                color=COLORS[query],
                linestyle=":",
                linewidth=1.5,
                zorder=2,
            )
            label_y = 10 ** (start + i * step) if len(logs) > 1 else ro_speedup
            ax.annotate(
                f"Q{query} RO {fmt_speedup(ro_speedup)}",
                xy=(2.0, ro_speedup),
                xytext=(2.08, label_y),
                textcoords="data",
                ha="left",
                va="center",
                fontsize=8,
                color=COLORS[query],
                arrowprops={
                    "arrowstyle": "-",
                    "color": COLORS[query],
                    "linewidth": 0.7,
                    "shrinkA": 0,
                    "shrinkB": 2,
                },
                annotation_clip=False,
            )
    ro_values = [v for _, v in ro_by_query]

    ax.axhline(1.0, color="#888888", linestyle="--", linewidth=1.0, zorder=1)
    ax.set_yscale("log")
    ax.set_xticks(xs)
    ax.set_xticklabels(["0.5s", "1s", "2s"], fontsize=11)
    ax.set_xlim(0.38, 2.0)
    span = all_values + ro_values
    lo = min(span) if span else 0.01
    hi = max(span) if span else 10
    ax.set_ylim(min(lo / 2.2, 0.005), hi * 2.2)
    ax.set_xlabel("Write interval (seconds between point writes)", fontsize=11)
    ax.set_ylabel("Workload speedup vs base (reads + writes, ×)", fontsize=11)
    ax.grid(True, which="both", linestyle=":", alpha=0.45, zorder=0)
    ax.legend(loc="best", framealpha=0.92, ncol=2, fontsize=10)
    ax.set_title(
        f"{backend} SEC-EDGAR SF3 aux speedup vs write interval\n"
        f"{subtitle}  ·  dotted = read-only speedup",
        fontsize=12,
        pad=12,
    )
    fig.tight_layout()
    fig.subplots_adjust(right=0.78)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    print(f"wrote {out_path}")


def duckdb_q1_parallel_rows() -> list[dict[str, str]]:
    """Q1 finished in per-interval logs but was never appended to the fair CSV."""
    rows: list[dict[str, str]] = []
    log_dir = RESULTS / "parallel_sec_edgar_fair_logs"
    for interval in ("0", "0.5", "1", "2"):
        log = log_dir / f"duckdb_sec_edgar_q1_{interval}.log"
        if not log.is_file():
            continue
        for line in log.read_text().splitlines():
            if not line.startswith("duckdb,sec_edgar,1,"):
                continue
            fields = line.split(",")
            rows.append(
                {
                    "engine": fields[0],
                    "workload": fields[1],
                    "query": fields[2],
                    "write_interval_sec": fields[3],
                    "mode": fields[4],
                    "reads": fields[5],
                    "writes": fields[6],
                    "rows": fields[7],
                    "read_p50_us": fields[8],
                    "read_sum_us": fields[9],
                    "write_p50_us": fields[10],
                    "write_sum_us": fields[11],
                    "build_us": fields[12],
                    "workload_sum_us": fields[13],
                    "workload_speedup_vs_base": fields[14],
                    "workload_with_build_us": fields[15],
                    "workload_with_build_speedup_vs_base": fields[16],
                }
            )
    return rows


def index_parallel_fair(
    path: Path, extra_rows: list[dict[str, str]] | None = None
) -> dict[str, dict[tuple[int, float], dict[str, float]]]:
    latest: dict[tuple[str, int, float, str], dict[str, str]] = {}
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if extra_rows:
        rows.extend(extra_rows)
    for row in rows:
        if row.get("workload") != "sec_edgar":
            continue
        key = (
            row["engine"],
            int(row["query"]),
            float(row["write_interval_sec"]),
            row["mode"],
        )
        latest[key] = row

    out: dict[str, dict[tuple[int, float], dict[str, float]]] = {
        "duckdb": {},
        "postgres": {},
    }
    pairs = {
        (engine, query, interval)
        for engine, query, interval, _mode in latest
        if engine in out
    }
    for engine, query, interval in pairs:
        base = latest.get((engine, query, interval, "base"))
        aux = latest.get((engine, query, interval, "aux_parallel"))
        if base is None or aux is None:
            continue
        base_read = float(base["read_p50_us"])
        aux_read = float(aux["read_p50_us"])
        if aux_read <= 0:
            continue
        out[engine][(query, interval)] = {
            "read": base_read / aux_read,
            "workload": float(aux["workload_speedup_vs_base"]),
            "base_read_us": base_read,
            "aux_read_us": aux_read,
        }
    return out


def main() -> None:
    duckdb = index_rows(load_summary(RESULTS / "duckdb_sec_edgar_aux_performance_summary.csv"))
    postgres = index_rows(
        postgres_with_q1(
            RESULTS / "postgres_aux_performance_summary.csv",
            RESULTS / "postgres_aux_combined_sweep.csv",
        )
    )

    plot_readonly_combined(
        duckdb,
        postgres,
        RESULTS / "sec_edgar_readonly_speedup.png",
    )
    plot_write_interval(
        duckdb,
        "DuckDB",
        RESULTS / "duckdb_sec_edgar_write_interval_speedup.png",
        "shared aux vs DuckDB base  ·  intervals 0.5 / 1 / 2s",
    )
    plot_write_interval(
        postgres,
        "Postgres",
        RESULTS / "postgres_sec_edgar_write_interval_speedup.png",
        "shared aux vs Postgres base  ·  intervals 0.5 / 1 / 2s",
    )

    parallel = index_parallel_fair(
        RESULTS / "parallel_sec_edgar_fair_sweep.csv",
        extra_rows=duckdb_q1_parallel_rows(),
    )
    plot_readonly_combined(
        parallel["duckdb"],
        parallel["postgres"],
        RESULTS / "parallel_sec_edgar_readonly_speedup.png",
        title="SEC-EDGAR SF3 aux_parallel speedup (read-only, t=8)",
        subtitle_prefix="aux_parallel vs host base",
        gendb_ms=GENDDB_MT_MS,
    )
    plot_write_interval(
        parallel["duckdb"],
        "DuckDB (t=8)",
        RESULTS / "duckdb_sec_edgar_parallel_write_interval_speedup.png",
        "aux_parallel vs DuckDB base  ·  intervals 0.5 / 1 / 2s",
    )
    plot_write_interval(
        parallel["postgres"],
        "Postgres (t=8)",
        RESULTS / "postgres_sec_edgar_parallel_write_interval_speedup.png",
        "aux_parallel vs Postgres base  ·  intervals 0.5 / 1 / 2s",
    )


if __name__ == "__main__":
    main()
