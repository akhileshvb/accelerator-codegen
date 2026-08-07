#!/usr/bin/env python3
"""Merge Postgres TPC-H and SEC EDGAR aux performance sweep results."""

from __future__ import annotations

import csv
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "benchmarks/aux_state/results"
SWEEP_CSV = RESULTS / "postgres_aux_performance_sweep.csv"
LOG_DIR = RESULTS / "postgres_aux_performance_logs"
OUT_CSV = RESULTS / "postgres_aux_combined_sweep.csv"

HEADER = [
    "engine",
    "workload",
    "query",
    "write_interval_sec",
    "mode",
    "reads",
    "writes",
    "rows",
    "read_p50_us",
    "read_sum_us",
    "write_p50_us",
    "write_sum_us",
    "build_us",
    "workload_sum_us",
    "workload_speedup_vs_base",
    "workload_with_build_us",
    "workload_with_build_speedup_vs_base",
    "read_speedup_p50",
]

ROW_RE = re.compile(r"^postgres,(tpch|sec_edgar),\d+,")


def parse_data_line(line: str) -> list[str] | None:
    line = line.strip()
    if not ROW_RE.match(line):
        return None
    return line.split(",")


def load_csv(path: Path) -> list[list[str]]:
    if not path.exists():
        return []
    rows: list[list[str]] = []
    with path.open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            return rows
        for row in reader:
            if row and row[0] == "postgres":
                rows.append(row)
    return rows


def load_logs(log_dir: Path) -> list[list[str]]:
    rows: list[list[str]] = []
    if not log_dir.exists():
        return rows
    for path in sorted(log_dir.glob("postgres_*.log")):
        for line in path.read_text().splitlines():
            parsed = parse_data_line(line)
            if parsed is not None:
                rows.append(parsed)
    return rows


def row_key(row: list[str]) -> tuple[str, str, str, str, str]:
    return (row[0], row[1], row[2], row[3], row[4])


def add_read_speedups(rows: list[list[str]]) -> list[list[str]]:
    base_read: dict[tuple[str, str, str], float] = {}
    for row in rows:
        if row[4] != "base":
            continue
        key = (row[1], row[2], row[3])
        base_read[key] = float(row[8])

    out: list[list[str]] = []
    for row in rows:
        extended = row + [""]
        if row[4] == "shared":
            key = (row[1], row[2], row[3])
            base = base_read.get(key)
            shared = float(row[8])
            if base is not None and shared > 0:
                extended[-1] = f"{base / shared:.4f}"
        out.append(extended)
    return out


def sort_key(row: list[str]) -> tuple[str, int, float, str]:
    workload_order = {"tpch": 0, "sec_edgar": 1}
    return (
        row[1],
        workload_order.get(row[1], 9),
        float(row[2]),
        float(row[3]),
        row[4],
    )


def main() -> int:
    merged: dict[tuple[str, str, str, str, str], list[str]] = {}
    for row in load_csv(SWEEP_CSV) + load_logs(LOG_DIR):
        merged[row_key(row)] = row

    rows = add_read_speedups(sorted(merged.values(), key=sort_key))

    OUT_CSV.parent.mkdir(parents=True, exist_ok=True)
    with OUT_CSV.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(HEADER)
        writer.writerows(rows)

    workloads = sorted({r[1] for r in rows})
    queries = sorted({(r[1], r[2]) for r in rows})
    print(f"wrote {OUT_CSV} ({len(rows)} rows, workloads={workloads}, queries={len(queries)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
