#!/usr/bin/env python3
"""Merge write-interval sweep rows by (query_num, write_interval_sec)."""

from __future__ import annotations

import argparse
from pathlib import Path

ROW_FIELDS = [
    "query_num",
    "write_frequency_pct",
    "write_interval_sec",
    "effective_write_pct",
    "writes_per_sec",
    "readers",
    "duration_sec",
    "aggregate_reader_qps",
    "reader_queries",
    "baseline_writer_commits",
    "accel_writer_commits",
    "baseline_read_sum_us",
    "accel_read_sum_us",
    "baseline_write_us",
    "accel_write_us",
    "workload_speedup",
    "read_speedup_p50",
    "read_speedup_p95",
    "baseline_read_p50_us",
    "accel_read_p50_us",
    "baseline_read_p95_us",
    "accel_read_p95_us",
    "accel_hit_rate",
]

CSV_HEADER = ["experiment", *ROW_FIELDS]


def parse_line(line: str) -> dict[str, str] | None:
    if line.startswith("experiment,"):
        return None
    if not line.startswith("write_interval,"):
        return None
    payload = line.split(",", 2)[2]
    fields = payload.split(",")
    if len(fields) < len(ROW_FIELDS):
        return None
    row = dict(zip(ROW_FIELDS, fields[: len(ROW_FIELDS)]))
    row["experiment"] = "write_interval"
    return row


def format_line(row: dict[str, str]) -> str:
    return "write_interval," + row["query_num"] + "," + ",".join(row[field] for field in ROW_FIELDS)


def load_rows(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    rows: dict[tuple[str, str], dict[str, str]] = {}
    if not path.exists():
        return rows
    for line in path.read_text().splitlines():
        row = parse_line(line)
        if row:
            rows[(row["query_num"], row["write_interval_sec"])] = row
    return rows


def write_rows(path: Path, rows: dict[tuple[str, str], dict[str, str]]) -> None:
    ordered = sorted(
        rows.values(),
        key=lambda r: (int(r["query_num"]), float(r["write_interval_sec"])),
    )
    lines = [",".join(CSV_HEADER)]
    lines.extend(format_line(row) for row in ordered)
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--update", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    merged = load_rows(args.base)
    merged.update(load_rows(args.update))
    write_rows(args.out, merged)
    print(f"wrote {len(merged)} rows to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
