#!/usr/bin/env python3
"""Calibrate aggregate reader QPS so writer-scheduled 50% lands near target effective %."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def parse_effective_pct(output: str) -> float:
    for line in output.splitlines():
        if not line.startswith("isolation,"):
            continue
        parts = line.split(",")
        if len(parts) < 12:
            continue
        try:
            return float(parts[10])
        except ValueError:
            continue
    return 0.0


def run_probe(
    *,
    bin_path: Path,
    db_path: Path,
    duration_sec: int,
    readers: int,
    queries: str,
    writer_target: str,
    aggregate_reader_qps: float,
) -> float:
    env = os.environ.copy()
    env.update(
        {
            "ACCEL_CROSS_ACCEL_QUERIES": queries,
            "ACCEL_CROSS_ACCEL_READERS": str(readers),
            "ACCEL_CROSS_ACCEL_DURATION_SEC": str(duration_sec),
            "ACCEL_CROSS_ACCEL_WRITER_SCHEDULED_WRITE_PCT": "50",
            "ACCEL_CROSS_ACCEL_WRITER_TARGET": writer_target,
            "ACCEL_CROSS_ACCEL_AGGREGATE_READER_QPS": f"{aggregate_reader_qps:.6f}",
            "ACCEL_CROSS_ACCEL_READER_PACE_US": "0",
            "ACCEL_CROSS_ACCEL_DISABLE_WRITER": "1",
            "ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS": "1",
            "ACCEL_CROSS_ACCEL_SKIP_BUILD": "0",
        }
    )
    proc = subprocess.run(
        [str(bin_path), str(db_path), "isolation"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    output = proc.stdout + "\n" + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"calibration probe failed at qps={aggregate_reader_qps}: {proc.stderr[-500:]}")
    return parse_effective_pct(output)


def calibrate(
    *,
    bin_path: Path,
    db_path: Path,
    duration_sec: int,
    readers: int,
    queries: str,
    writer_target: str,
    target_effective_pct: float,
) -> float:
    lo = 1.0
    hi = 5000.0
    best_qps = lo
    best_delta = float("inf")

    for _ in range(14):
        mid = (lo + hi) / 2.0
        effective = run_probe(
            bin_path=bin_path,
            db_path=db_path,
            duration_sec=duration_sec,
            readers=readers,
            queries=queries,
            writer_target=writer_target,
            aggregate_reader_qps=mid,
        )
        delta = abs(effective - target_effective_pct)
        if delta < best_delta:
            best_delta = delta
            best_qps = mid
        if effective < target_effective_pct - 1.0:
            hi = mid
        else:
            lo = mid

    return best_qps


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--duration-sec", type=int, default=15)
    parser.add_argument("--readers", type=int, default=6)
    parser.add_argument("--queries", default="1,3,6,10,12,14")
    parser.add_argument("--writer-target", default="round_robin")
    parser.add_argument("--target-effective-pct", type=float, default=50.0)
    args = parser.parse_args()

    qps = calibrate(
        bin_path=args.bin,
        db_path=args.db,
        duration_sec=args.duration_sec,
        readers=args.readers,
        queries=args.queries,
        writer_target=args.writer_target,
        target_effective_pct=args.target_effective_pct,
    )
    print(f"{qps:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
