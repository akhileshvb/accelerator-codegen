#!/usr/bin/env python3
"""Matched PostgreSQL base/shared-interface read/write interval benchmark.

Supports:
  --write-mode point  (default): probe UPDATE + restore
  --write-mode rf     : alternating RF2 delete / RF1 insert mini-chunks
  --duration-sec N    : wall-clock measured window (overrides --reads when >0)
"""

from __future__ import annotations

import argparse
import os
import statistics
import time

from postgres_differential import connect, differential, run_query, collect_invalidating_probe
from shared_query_catalog import lookup_query
from tpch_rf import apply_rf_chunk

CSV_HEADER = (
    "engine,workload,query,write_mode,write_interval_sec,mode,reads,writes,rows,"
    "read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,"
    "workload_sum_us,workload_speedup_vs_base,workload_with_build_us,"
    "workload_with_build_speedup_vs_base,measured_duration_sec,rf_chunk_orders"
)


def measure_mode(
    db,
    entry,
    interval,
    shared,
    read_operations,
    reader_qps,
    warmup,
    *,
    write_mode: str,
    rf_chunk_orders: int,
    duration_sec: float,
):
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
        cur.execute("SET work_mem = '512MB'")
        cur.execute("SET jit = off")
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
        if do_writes and write_mode == "point":
            progress("select invalidating probe")
            probe = collect_invalidating_probe(conn, cur, entry)
            print(
                f"invalidating_write_probes=1 query={entry.query_id}",
                flush=True,
            )

        next_write = interval if do_writes else float("inf")
        reads, writes, rows = 0, 0, 0
        read_samples, write_samples = [], []
        write_seq = 0
        wall_start = time.perf_counter()
        use_wall = duration_sec > 0
        read_index = 0

        while True:
            if use_wall:
                elapsed = time.perf_counter() - wall_start
                if elapsed >= duration_sec:
                    break
                virtual_time = elapsed
            else:
                if read_index >= read_operations:
                    break
                virtual_time = read_index / reader_qps

            while do_writes and virtual_time + 1e-12 >= next_write:
                if use_wall and (time.perf_counter() - wall_start) >= duration_sec:
                    break
                start = time.perf_counter_ns()
                if write_mode == "rf":
                    apply_rf_chunk(cur, write_seq, rf_chunk_orders)
                    conn.commit()
                else:
                    cur.execute(probe["update"])
                    conn.commit()
                write_samples.append((time.perf_counter_ns() - start) / 1000.0)
                writes += 1
                write_seq += 1
                next_write += interval
                if use_wall:
                    virtual_time = time.perf_counter() - wall_start

            if use_wall and (time.perf_counter() - wall_start) >= duration_sec:
                break

            if not use_wall:
                progress(f"read {read_index + 1}/{read_operations}")
            start = time.perf_counter_ns()
            result = run_query(cur, statement)
            rows = len(result)
            read_samples.append((time.perf_counter_ns() - start) / 1000.0)
            reads += 1
            read_index += 1

        if write_mode == "point" and writes > 0 and probe is not None:
            progress("restore probe row")
            cur.execute(probe["restore"], probe["restore_args"])
            conn.commit()

        measured = time.perf_counter() - wall_start if use_wall else 0.0
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
            measured,
        )
    finally:
        conn.close()


def emit_mode_line(
    emit,
    workload,
    entry,
    interval,
    mode,
    values,
    speedup,
    build_speedup,
    write_mode,
    rf_chunk_orders,
):
    (
        reads,
        writes,
        rows,
        p50_read,
        read_sum,
        p50_write,
        write_sum,
        build_us,
        measured,
    ) = values
    workload_sum = read_sum + write_sum
    workload_with_build = build_us + workload_sum
    emit(
        "postgres,%s,%d,%s,%s,%s,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.2f,%.4f,%.3f,%d"
        % (
            workload,
            entry.query_id,
            write_mode,
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
            measured,
            rf_chunk_orders,
        )
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=("tpch", "sec_edgar"), required=True)
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--db", required=True)
    parser.add_argument("--reads", type=int, default=8)
    parser.add_argument("--reader-qps", type=float, default=2.0)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--output", default=None)
    parser.add_argument("--append", action="store_true")
    parser.add_argument("--skip-differential", action="store_true")
    parser.add_argument("--write-mode", choices=("point", "rf"), default="point")
    parser.add_argument("--rf-chunk-orders", type=int, default=1)
    parser.add_argument(
        "--duration-sec",
        type=float,
        default=0.0,
        help="If >0, wall-clock measure for this many seconds instead of --reads",
    )
    parser.add_argument(
        "--min-writes",
        type=int,
        default=5,
        help="Stretch duration to at least min_writes * interval (matches DuckDB)",
    )
    parser.add_argument("intervals", nargs="+", type=float)
    args = parser.parse_args()

    if args.write_mode == "rf" and args.workload != "tpch":
        raise SystemExit("RF write mode is only supported for tpch")

    entry = lookup_query(args.workload, args.query)
    rows = None
    if not args.skip_differential and args.write_mode == "point":
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
            run_duration = args.duration_sec
            if args.duration_sec > 0 and interval > 0 and args.min_writes > 0:
                run_duration = max(args.duration_sec, args.min_writes * interval)
            print(
                f"# interval={interval} duration_sec={run_duration} "
                f"write_mode={args.write_mode} rf_chunk_orders={args.rf_chunk_orders}",
                flush=True,
            )
            base = measure_mode(
                args.db,
                entry,
                interval,
                False,
                args.reads,
                args.reader_qps,
                args.warmup,
                write_mode=args.write_mode,
                rf_chunk_orders=args.rf_chunk_orders,
                duration_sec=run_duration,
            )
            shared = measure_mode(
                args.db,
                entry,
                interval,
                True,
                args.reads,
                args.reader_qps,
                args.warmup,
                write_mode=args.write_mode,
                rf_chunk_orders=args.rf_chunk_orders,
                duration_sec=run_duration,
            )
            base_workload = base[4] + base[6]
            shared_workload = shared[4] + shared[6]
            base_with_build = base[7] + base_workload
            shared_with_build = shared[7] + shared_workload
            emit_mode_line(
                emit,
                args.workload,
                entry,
                interval,
                "base",
                base,
                1.0,
                1.0,
                args.write_mode,
                args.rf_chunk_orders,
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
                args.write_mode,
                args.rf_chunk_orders,
            )
        if rows is not None:
            emit("differential_rows=%d" % rows)
    finally:
        if output:
            output.close()


if __name__ == "__main__":
    main()
