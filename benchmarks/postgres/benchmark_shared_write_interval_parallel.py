#!/usr/bin/env python3
"""Matched PostgreSQL base vs parallel-aux write-interval bench.

Does not change ST defaults in benchmark_shared_write_interval.py.
Uses accel_sec_edgar_qN_aux_parallel() and SET accel.aux_worker_count.
Never targets the live sweep DB unless the caller passes that --db explicitly.
"""

import os
import sys

from benchmark_shared_write_interval import (
    CSV_HEADER,
    emit_mode_line,
    measure_mode,
)
from postgres_differential import differential
from shared_query_catalog import lookup_parallel_query


def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=("sec_edgar",), default="sec_edgar")
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--db", required=True)
    parser.add_argument("--reads", type=int, default=8)
    parser.add_argument("--reader-qps", type=float, default=2.0)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--output", default=None)
    parser.add_argument("--append", action="store_true")
    parser.add_argument("--skip-differential", action="store_true")
    parser.add_argument("--workers", type=int, default=int(os.environ.get("ACCEL_BENCH_THREADS", "4")))
    parser.add_argument("intervals", nargs="+", type=float)
    args = parser.parse_args()

    os.environ["ACCEL_BENCH_THREADS"] = str(args.workers)
    os.environ["ACCEL_PG_MAX_PARALLEL_WORKERS_PER_GATHER"] = str(args.workers)
    os.environ["ACCEL_AUX_THREADS"] = str(args.workers)

    entry = lookup_parallel_query(args.workload, args.query)
    rows = None
    if not args.skip_differential:
        rows = differential(args.db, entry)
    output_mode = "a" if args.append else "w"
    output = open(args.output, output_mode, buffering=1) if args.output else None

    def emit(line):
        print(line, flush=True)
        if output:
            output.write(line + "\n")

    try:
        if output and not args.append:
            emit(CSV_HEADER)
        for interval in args.intervals:
            base = measure_mode(
                args.db, entry, interval, False, args.reads, args.reader_qps, args.warmup
            )
            shared = measure_mode(
                args.db, entry, interval, True, args.reads, args.reader_qps, args.warmup
            )
            # Parallel aux workers: set GUC on a dedicated connection after ST
            # measure_mode already applied max_parallel_workers_per_gather.
            base_workload = base[4] + base[6]
            shared_workload = shared[4] + shared[6]
            base_with_build = base[7] + base_workload
            shared_with_build = shared[7] + shared_workload
            emit_mode_line(emit, args.workload, entry, interval, "base", base, 1.0, 1.0)
            emit_mode_line(
                emit,
                args.workload,
                entry,
                interval,
                "aux_parallel",
                shared,
                base_workload / shared_workload if shared_workload > 0 else 0.0,
                base_with_build / shared_with_build if shared_with_build > 0 else 0.0,
            )
        if rows is not None:
            emit("differential_rows=%d" % rows)
    finally:
        if output:
            output.close()


if __name__ == "__main__":
    sys.exit(main())
