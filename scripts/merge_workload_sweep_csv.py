#!/usr/bin/env python3
"""Merge per-query workload sweep rows into a combined CSV (replace by query_num)."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

HEADER = [
    "experiment",
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


def parse_row(line: str) -> dict[str, str] | None:
    if not line.startswith("per_query_workload,"):
        return None
    parts = line.split(",")
    if len(parts) == len(HEADER) + 1 and parts[1] == parts[2]:
        # Legacy awk rows: per_query_workload,<q>,<q>,<freq>,...
        data = dict(zip(HEADER, [parts[0], parts[1], *parts[3:]]))
    elif len(parts) >= len(HEADER):
        data = dict(zip(HEADER, parts[: len(HEADER)]))
    else:
        payload = line.split(",", 2)[2]
        fields = payload.split(",")
        if len(fields) < len(HEADER) - 1:
            return None
        data = dict(zip(HEADER[1:], fields))
        data["experiment"] = "per_query_workload"
    data["experiment"] = "per_query_workload"
    return data


def load_rows(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    rows: dict[tuple[str, str], dict[str, str]] = {}
    if not path.exists():
        return rows
    text = path.read_text().splitlines()
    if not text:
        return rows
    legacy = any(line.startswith("per_query_workload,") for line in text[1:])
    if not legacy and text[0].startswith("experiment,") and "write_frequency_pct" in text[0]:
        for row in csv.DictReader(text):
            if row.get("query_num") and row.get("write_frequency_pct") is not None:
                rows[(row["query_num"], row["write_frequency_pct"])] = {
                    k: row[k] for k in HEADER if k in row and row[k] is not None
                }
        return rows
    for line in text[1:]:
        row = parse_row(line) if line.startswith("per_query_workload,") else None
        if row:
            rows[(row["query_num"], row["write_frequency_pct"])] = row
    return rows


def write_rows(path: Path, rows: dict[tuple[str, str], dict[str, str]]) -> None:
    ordered = sorted(rows.values(), key=lambda r: (int(r["query_num"]), int(r["write_frequency_pct"])))
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=HEADER)
        writer.writeheader()
        writer.writerows(ordered)


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
