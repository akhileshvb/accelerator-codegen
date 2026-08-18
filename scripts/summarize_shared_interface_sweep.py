#!/usr/bin/env python3
"""Summarize shared-interface aux performance sweep results."""

from __future__ import annotations

import argparse
import csv
import os
import sys
from collections import defaultdict
from pathlib import Path

WORKLOAD_DATASETS = {
    "tpch": os.environ.get("ACCEL_SWEEP_DATASET", "tpch_sf1"),
    "sec_edgar": os.environ.get("ACCEL_SEC_SWEEP_DATASET", "sec_edgar_sf3"),
}

TPCH_QUERIES = {1, 3, 6, 9, 10, 11, 12, 14, 17, 18, 21}


def fmt_speedup(value: float) -> str:
    if value <= 0:
        return "—"
    if value >= 100:
        return f"{value:.0f}x"
    if value >= 10:
        return f"{value:.1f}x"
    return f"{value:.2f}x"


def infer_workload(row: dict[str, str]) -> str:
    if row.get("workload"):
        return row["workload"]
    query = int(row["query"])
    rows = int(row.get("rows") or 0)
    if query in TPCH_QUERIES:
        if query == 1 and rows > 10:
            return "sec_edgar"
        return "tpch"
    return "sec_edgar"


def load_rows(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            if not row.get("engine") or row.get("mode") not in {"base", "shared"}:
                continue
            row["workload"] = infer_workload(row)
            if not row.get("build_us"):
                row["build_us"] = "0"
            rows.append(row)
    return rows


def summarize_rows(rows: list[dict[str, str]]) -> list[dict[str, str | float]]:
    grouped: dict[tuple[str, str, str, str], dict[str, dict[str, str]]] = defaultdict(
        dict
    )
    for row in rows:
        key = (
            row["engine"],
            row["workload"],
            row["query"],
            row["write_interval_sec"],
        )
        grouped[key][row["mode"]] = row

    summary: list[dict[str, str | float]] = []
    for (engine, workload, query, interval), modes in sorted(
        grouped.items(),
        key=lambda item: (
            item[0][0],
            item[0][1],
            float(item[0][3]),
            int(item[0][2]),
        ),
    ):
        if "base" not in modes or "shared" not in modes:
            continue
        base = modes["base"]
        shared = modes["shared"]
        base_read_p50 = float(base["read_p50_us"])
        shared_read_p50 = float(shared["read_p50_us"])
        base_workload = float(base["read_sum_us"]) + float(base["write_sum_us"])
        shared_workload = float(shared["read_sum_us"]) + float(shared["write_sum_us"])
        shared_build_us = float(shared.get("build_us") or 0)
        base_with_build = float(base.get("build_us") or 0) + base_workload
        shared_with_build = shared_build_us + shared_workload
        read_speedup = base_read_p50 / shared_read_p50 if shared_read_p50 > 0 else 0.0
        workload_speedup = (
            base_workload / shared_workload if shared_workload > 0 else 0.0
        )
        workload_with_build_speedup = (
            base_with_build / shared_with_build if shared_with_build > 0 else 0.0
        )
        summary.append(
            {
                "engine": engine,
                "workload": workload,
                "dataset": WORKLOAD_DATASETS[workload],
                "query": int(query),
                "write_interval_sec": float(interval),
                "reads": int(base["reads"]),
                "writes": int(base["writes"]),
                "rows": int(base["rows"]),
                "shared_build_us": shared_build_us,
                "base_read_p50_us": base_read_p50,
                "shared_read_p50_us": shared_read_p50,
                "base_workload_us": base_workload,
                "shared_workload_us": shared_workload,
                "shared_workload_with_build_us": shared_with_build,
                "read_speedup": read_speedup,
                "workload_speedup": workload_speedup,
                "workload_with_build_speedup": workload_with_build_speedup,
            }
        )
    return summary


def write_summary(path: Path, summary: list[dict[str, str | float]]) -> None:
    fieldnames = [
        "engine",
        "workload",
        "dataset",
        "query",
        "write_interval_sec",
        "reads",
        "writes",
        "rows",
        "shared_build_us",
        "base_read_p50_us",
        "shared_read_p50_us",
        "base_workload_us",
        "shared_workload_us",
        "shared_workload_with_build_us",
        "read_speedup",
        "workload_speedup",
        "workload_with_build_speedup",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in summary:
            writer.writerow(row)


def print_report(summary: list[dict[str, str | float]]) -> None:
    print("=== Shared aux performance summary ===")
    print(
        "Steady-state workload_speedup excludes shared build cost. "
        "workload_with_build_speedup adds shared build_us (DuckDB Build(); "
        "Postgres cold first accel read) to the shared timed workload."
    )
    print()
    for engine in ("duckdb", "postgres"):
        engine_rows = [row for row in summary if row["engine"] == engine]
        if not engine_rows:
            continue
        print(f"[{engine}]")
        for workload in ("tpch", "sec_edgar"):
            workload_rows = [row for row in engine_rows if row["workload"] == workload]
            if not workload_rows:
                continue
            print(f"  {workload} ({WORKLOAD_DATASETS[workload]})")
            for row in workload_rows:
                interval = row["write_interval_sec"]
                label = "read-only" if interval == 0 else f"interval={interval:g}s"
                build_s = float(row["shared_build_us"]) / 1e6
                print(
                    f"    Q{row['query']} {label}: "
                    f"read {fmt_speedup(float(row['read_speedup']))}, "
                    f"workload {fmt_speedup(float(row['workload_speedup']))}, "
                    f"with_build {fmt_speedup(float(row['workload_with_build_speedup']))} "
                    f"(build {build_s:.2f}s)"
                )
        print()


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize shared aux sweep CSV")
    parser.add_argument("csv", type=Path)
    parser.add_argument(
        "--summary",
        type=Path,
        default=None,
        help="optional path to write normalized summary CSV",
    )
    args = parser.parse_args()

    if not args.csv.exists():
        print(f"missing sweep csv: {args.csv}", file=sys.stderr)
        return 1

    summary = summarize_rows(load_rows(args.csv))
    if not summary:
        print(f"no benchmark rows found in {args.csv}", file=sys.stderr)
        return 1

    if args.summary:
        write_summary(args.summary, summary)
        print(f"wrote summary to {args.summary}")

    print_report(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
