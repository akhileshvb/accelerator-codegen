#!/usr/bin/env python3
"""Cold and incremental differential checks for shared PostgreSQL queries."""

from __future__ import annotations

import argparse
import re
import sys

from postgres_differential import connect, differential, incremental_differential, rf_differential
from shared_query_catalog import lookup_query, queries_for_workload, tpch_rf_query_ids


def _aux_proc_name(aux_sql: str) -> str | None:
    match = re.search(r"accel_[a-z0-9_]+_aux", aux_sql)
    return match.group(0) if match else None


def aux_function_installed(db: str, entry) -> bool:
    name = _aux_proc_name(entry.aux_sql)
    if not name:
        return False
    conn = connect(db)
    cur = conn.cursor()
    try:
        cur.execute("SELECT 1 FROM pg_proc WHERE proname = %s", (name,))
        return cur.fetchone() is not None
    finally:
        conn.close()


def run_checks(
    workload: str,
    query_ids: list[int],
    db: str,
    *,
    cold: bool,
    incremental: bool,
    rf: bool,
) -> int:
    failures = 0
    for query_id in query_ids:
        entry = lookup_query(workload, query_id)
        if not aux_function_installed(db, entry):
            print(
                f"=== SKIP postgres {workload} Q{query_id}: "
                f"{_aux_proc_name(entry.aux_sql)} not installed on {db} ===",
                flush=True,
            )
            continue
        if cold:
            label = f"postgres {workload} Q{query_id} cold differential"
            print(f"=== {label} ===", flush=True)
            try:
                rows = differential(db, entry)
                print(f"PASS ({rows} rows)", flush=True)
            except Exception as exc:
                print(f"FAIL: {exc}", flush=True)
                failures += 1
        if incremental:
            label = f"postgres {workload} Q{query_id} incremental differential"
            print(f"=== {label} ===", flush=True)
            try:
                rows = incremental_differential(db, entry)
                print(f"PASS ({rows} rows)", flush=True)
            except Exception as exc:
                print(f"FAIL: {exc}", flush=True)
                failures += 1
        if rf:
            if workload != "tpch":
                print(
                    f"=== SKIP postgres {workload} Q{query_id} RF differential (TPC-H only) ===",
                    flush=True,
                )
                continue
            if query_id == 11:
                print(
                    "=== SKIP postgres tpch Q11 RF differential (RF does not touch partsupp) ===",
                    flush=True,
                )
                continue
            label = f"postgres {workload} Q{query_id} RF differential"
            print(f"=== {label} ===", flush=True)
            try:
                rows = rf_differential(db, entry)
                print(f"PASS ({rows} rows)", flush=True)
            except Exception as exc:
                print(f"FAIL: {exc}", flush=True)
                failures += 1
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=("tpch", "sec_edgar"))
    parser.add_argument("--query", type=int, default=None)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--db", required=True)
    parser.add_argument(
        "--mode",
        choices=("cold", "incremental", "rf", "both", "all"),
        default="both",
    )
    args = parser.parse_args()

    if args.all and args.query is not None:
        parser.error("use either --all or --query, not both")
    if not args.all and args.query is None:
        parser.error("specify --query or --all")

    if args.mode == "rf" and args.all and args.workload == "tpch":
        query_ids = tpch_rf_query_ids()
    else:
        query_ids = queries_for_workload(args.workload) if args.all else [args.query]
    cold = args.mode in ("cold", "both", "all")
    incremental = args.mode in ("incremental", "both", "all")
    rf = args.mode in ("rf", "all")
    if args.mode == "rf":
        cold = False
        incremental = False

    failures = run_checks(
        args.workload,
        query_ids,
        args.db,
        cold=cold,
        incremental=incremental,
        rf=rf,
    )
    if failures:
        print(f"{failures} check(s) failed", file=sys.stderr)
        return 1
    print(f"all {len(query_ids)} {args.workload} {args.mode} check(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
