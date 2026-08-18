#!/usr/bin/env python3
"""Matched PostgreSQL base/shared-interface read/write interval benchmark."""

import argparse
import os
import statistics
import time

from postgres_differential import connect, differential, run_query, collect_invalidating_probe
from shared_query_catalog import lookup_query, queries_for_workload

CSV_HEADER = (
    "engine,workload,query,write_interval_sec,mode,reads,writes,rows,"
    "read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,"
    "workload_sum_us,workload_speedup_vs_base,workload_with_build_us,"
    "workload_with_build_speedup_vs_base"
)


def measure_mode(db, entry, interval, shared, read_operations, reader_qps, warmup):
    mode = "shared" if shared else "base"
    conn = connect(db)
    conn.autocommit = False
    cur = conn.cursor()
    build_us = 0.0
    try:
        workers = int(
            os.environ.get(
                "ACCEL_BENCH_THREADS",
                os.environ.get("ACCEL_PG_MAX_PARALLEL_WORKERS_PER_GATHER", "0"),
            )
        )

        def apply_worker_gucs():
            # Same worker cap for Postgres SQL (gather) and aux (GUC / env).
            try:
                cur.execute("SET max_parallel_workers = %s", (workers,))
            except Exception:
                conn.rollback()
            cur.execute("SET max_parallel_workers_per_gather = %s", (workers,))
            if workers > 0:
                try:
                    cur.execute("SET accel.aux_worker_count = %s", (workers,))
                except Exception:
                    conn.rollback()
                    try:
                        cur.execute("SET max_parallel_workers = %s", (workers,))
                    except Exception:
                        conn.rollback()
                    cur.execute(
                        "SET max_parallel_workers_per_gather = %s", (workers,)
                    )

        apply_worker_gucs()
        if not shared:
            cur.execute("SET session_replication_role = replica")
        statement = entry.aux_sql if shared else entry.base_sql

        def progress(msg):
            print(
                f"[progress] q{entry.query_id} interval={interval} mode={mode}: {msg}",
                flush=True,
            )

        if shared:
            progress("cold build starting")
            build_start = time.perf_counter_ns()
            run_query(cur, statement)
            conn.commit()
            build_us = (time.perf_counter_ns() - build_start) / 1000.0
            progress(f"cold build done ({build_us / 1e6:.1f}s)")

        for warm_i in range(warmup):
            progress(f"warmup {warm_i + 1}/{warmup}")
            run_query(cur, statement)
            conn.commit()

        do_writes = interval > 0
        probe = None
        if do_writes:
            progress("select invalidating probe")
            probe = collect_invalidating_probe(conn, cur, entry)
            print(
                f"invalidating_write_probes=1 query={entry.query_id}",
                flush=True,
            )
        update = probe["update"] if probe else None
        restore = probe["restore"] if probe else None
        restore_args = probe["restore_args"] if probe else None

        next_write = interval if do_writes else float("inf")
        reads, writes, rows = 0, 0, 0
        read_samples, write_samples = [], []
        for read_index in range(read_operations):
            virtual_time = read_index / reader_qps
            while do_writes and virtual_time + 1e-12 >= next_write:
                start = time.perf_counter_ns()
                cur.execute("BEGIN")
                cur.execute(update)
                conn.commit()
                write_samples.append((time.perf_counter_ns() - start) / 1000.0)
                writes += 1
                next_write += interval
            progress(f"read {read_index + 1}/{read_operations}")
            start = time.perf_counter_ns()
            result = run_query(cur, statement)
            rows = len(result)
            read_samples.append((time.perf_counter_ns() - start) / 1000.0)
            reads += 1

        if writes > 0:
            progress("restore probe row")
            cur.execute(restore, restore_args)
            conn.commit()
        p50_read = statistics.median(read_samples) if read_samples else 0.0
        p50_write = statistics.median(write_samples) if write_samples else 0.0
        return (
            reads,
            writes,
            rows,
            p50_read,
            sum(read_samples),
            p50_write,
            sum(write_samples),
            build_us,
        )
    finally:
        conn.close()


def emit_mode_line(emit, workload, entry, interval, mode, values, speedup, build_speedup):
    reads, writes, rows, p50_read, read_sum, p50_write, write_sum, build_us = values
    workload_sum = read_sum + write_sum
    workload_with_build = build_us + workload_sum
    emit(
        "postgres,%s,%d,%s,%s,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.2f,%.4f"
        % (
            workload,
            entry.query_id,
            interval,
            mode,
            reads,
            writes,
            rows,
            p50_read,
            read_sum,
            p50_write,
            write_sum,
            build_us,
            workload_sum,
            speedup,
            workload_with_build,
            build_speedup,
        )
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=("tpch", "sec_edgar"), required=True)
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--db", required=True)
    parser.add_argument("--reads", type=int, default=8,
                        help="number of timed reads per mode")
    parser.add_argument("--reader-qps", type=float, default=2.0)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--output", default=None,
                        help="also write results to this file while running")
    parser.add_argument("--append", action="store_true",
                        help="append rows to --output instead of truncating")
    parser.add_argument("--skip-differential", action="store_true",
                        help="skip the pre-benchmark correctness check")
    parser.add_argument("intervals", nargs="+", type=float)
    args = parser.parse_args()

    entry = lookup_query(args.workload, args.query)
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
            base = measure_mode(args.db, entry, interval, False,
                                args.reads, args.reader_qps, args.warmup)
            shared = measure_mode(args.db, entry, interval, True,
                                  args.reads, args.reader_qps, args.warmup)
            base_workload = base[4] + base[6]
            shared_workload = shared[4] + shared[6]
            base_with_build = base[7] + base_workload
            shared_with_build = shared[7] + shared_workload
            emit_mode_line(
                emit, args.workload, entry, interval, "base", base, 1.0, 1.0
            )
            emit_mode_line(
                emit,
                args.workload,
                entry,
                interval,
                "shared",
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
    main()
