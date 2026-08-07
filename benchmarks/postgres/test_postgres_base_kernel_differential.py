#!/usr/bin/env python3
"""Cold differential checks for native PostgreSQL base kernels (no aux state)."""

from __future__ import annotations

import argparse
import sys

from postgres_differential import base_kernel_differential
from shared_query_catalog import base_kernel_queries_for_workload, lookup_base_kernel_query


def run_checks(workload: str, query_ids: list[int], db: str) -> int:
    failures = 0
    for query_id in query_ids:
        entry = lookup_base_kernel_query(workload, query_id)
        label = f"postgres {workload} Q{query_id} base-kernel differential"
        print(f"=== {label} ===")
        try:
            rows = base_kernel_differential(db, entry)
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
    args = parser.parse_args()

    if args.all and args.query is not None:
        parser.error("use either --all or --query, not both")
    if not args.all and args.query is None:
        parser.error("specify --query or --all")

    query_ids = base_kernel_queries_for_workload(args.workload) if args.all else [args.query]
    failures = run_checks(args.workload, query_ids, args.db)
    if failures:
        print(f"{failures} check(s) failed", file=sys.stderr)
        return 1
    print(f"all {len(query_ids)} {args.workload} base-kernel check(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
