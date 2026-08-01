#!/usr/bin/env python3
"""Calibrate aggregate reader QPS for single-query workload multi-connection benchmark."""

from __future__ import annotations

import argparse
import math
import os
import subprocess
from pathlib import Path


def parse_effective_pct(output: str) -> float:
    for line in output.splitlines():
        if line.startswith("query_num,"):
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            int(parts[0])
            return float(parts[3])
        except ValueError:
            continue
    return 0.0


def parse_reader_queries(output: str) -> int:
    for line in output.splitlines():
        if line.startswith("query_num,"):
            continue
        parts = line.split(",")
        if len(parts) < 8:
            continue
        try:
            int(parts[0])
            return int(float(parts[8]))
        except ValueError:
            continue
    return 0


def run_probe(
    *,
    bin_path: Path,
    db_path: Path,
    query: int,
    duration_sec: int,
    readers: int,
    aggregate_reader_qps: float,
    write_pct: int,
) -> tuple[float, int]:
    env = os.environ.copy()
    env.update(
        {
            "ACCEL_WORKLOAD_QUERY": str(query),
            "ACCEL_WORKLOAD_READERS": str(readers),
            "ACCEL_WORKLOAD_DURATION_SEC": str(duration_sec),
            "ACCEL_WORKLOAD_WRITE_PCT": str(write_pct),
            "ACCEL_WORKLOAD_AGGREGATE_READER_QPS": f"{aggregate_reader_qps:.6f}",
            "ACCEL_WORKLOAD_SKIP_CORRECTNESS": "1",
            "ACCEL_WORKLOAD_SKIP_BUILD": "0",
            "ACCEL_WORKLOAD_WARMUP": "3",
        }
    )
    proc = subprocess.run(
        [str(bin_path), str(query), str(db_path)],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    output = proc.stdout + "\n" + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"calibration probe failed q={query} qps={aggregate_reader_qps}: {proc.stderr[-500:]}")
    effective = parse_effective_pct(output)
    reader_queries = parse_reader_queries(output)
    return effective, reader_queries


def calibrate_read_throughput(
    *,
    bin_path: Path,
    db_path: Path,
    query: int,
    duration_sec: int,
    readers: int,
) -> float:
    lo = 1.0
    hi = 500.0
    best_qps = lo
    for _ in range(12):
        mid = (lo + hi) / 2.0
        _, reader_queries = run_probe(
            bin_path=bin_path,
            db_path=db_path,
            query=query,
            duration_sec=duration_sec,
            readers=readers,
            aggregate_reader_qps=mid,
            write_pct=0,
        )
        target = mid * duration_sec * 0.85
        if reader_queries < target:
            hi = mid
        else:
            lo = mid
            best_qps = mid
    return best_qps


def calibrate(
    *,
    bin_path: Path,
    db_path: Path,
    query: int,
    duration_sec: int,
    readers: int,
    target_effective_pct: float,
    write_pct: int,
) -> float:
    lo = 1.0
    hi = 120.0
    best_qps = lo
    best_delta = float("inf")
    for _ in range(12):
        mid = (lo + hi) / 2.0
        effective, _ = run_probe(
            bin_path=bin_path,
            db_path=db_path,
            query=query,
            duration_sec=duration_sec,
            readers=readers,
            aggregate_reader_qps=mid,
            write_pct=write_pct,
        )
        delta = abs(effective - target_effective_pct)
        if delta < best_delta:
            best_delta = delta
            best_qps = mid
        if effective < target_effective_pct - 1.0:
            hi = mid
        else:
            lo = mid

    # Ensure enough reader completions for a 1% schedule (stride 100) during measured runs.
    min_qps_for_1pct = 100.0 / max(duration_sec, 1)
    return max(best_qps, min_qps_for_1pct)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--duration-sec", type=int, default=12)
    parser.add_argument("--readers", type=int, default=4)
    parser.add_argument("--target-effective-pct", type=float, default=50.0)
    parser.add_argument("--write-pct", type=int, default=50)
    parser.add_argument("--mode", choices=("write-schedule", "read-throughput"), default="write-schedule")
    args = parser.parse_args()
    if args.mode == "read-throughput":
        qps = calibrate_read_throughput(
            bin_path=args.bin,
            db_path=args.db,
            query=args.query,
            duration_sec=args.duration_sec,
            readers=args.readers,
        )
    else:
        qps = calibrate(
            bin_path=args.bin,
            db_path=args.db,
            query=args.query,
            duration_sec=args.duration_sec,
            readers=args.readers,
            target_effective_pct=args.target_effective_pct,
            write_pct=args.write_pct,
        )
    print(f"{qps:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
