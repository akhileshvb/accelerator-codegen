#!/usr/bin/env python3
"""Cold and incremental differential checks for shared PostgreSQL queries."""

from __future__ import annotations

import argparse
import sys

from postgres_differential import differential, incremental_differential
from shared_query_catalog import lookup_query, queries_for_workload


def run_checks(
    workload: str,
    query_ids: list[int],
    db: str,
    *,
    cold: bool,
    incremental: bool,
) -> int:
    failures = 0
    for query_id in query_ids:
        entry = lookup_query(workload, query_id)
        if cold:
            label = f"postgres {workload} Q{query_id} cold differential"
            print(f"=== {label} ===")
            try:
                rows = differential(db, entry)
                print(f"PASS ({rows} rows)")
            except Exception as exc:
                print(f"FAIL: {exc}")
                failures += 1
        if incremental:
            label = f"postgres {workload} Q{query_id} incremental differential"
            print(f"=== {label} ===")
            try:
                rows = incremental_differential(db, entry)
                print(f"PASS ({rows} rows)")
            except Exception as exc:
                print(f"FAIL: {exc}")
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
        choices=("cold", "incremental", "both"),
        default="both",
    )
    args = parser.parse_args()

    if args.all and args.query is not None:
        parser.error("use either --all or --query, not both")
    if not args.all and args.query is None:
        parser.error("specify --query or --all")

    query_ids = queries_for_workload(args.workload) if args.all else [args.query]
    cold = args.mode in ("cold", "both")
    incremental = args.mode in ("incremental", "both")

    failures = run_checks(
        args.workload,
        query_ids,
        args.db,
        cold=cold,
        incremental=incremental,
    )
    if failures:
        print(f"{failures} check(s) failed", file=sys.stderr)
        return 1
    print(f"all {len(query_ids)} {args.workload} {args.mode} check(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
