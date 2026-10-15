#!/usr/bin/env python3
"""Create a small TPC-H DuckDB for fast shared-interface tests."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import duckdb

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb"
DEFAULT_DEST = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_test.duckdb"

SUBSET_SQL = ROOT / "workloads" / "tpch" / "test_db_subset.sql"


def subset_statements() -> list[str]:
    """The statements of workloads/tpch/test_db_subset.sql (shared with ctest's
    make_tpch_test_db, so both build the same subset)."""
    text = re.sub(r"--[^\n]*", "", SUBSET_SQL.read_text())
    return [stmt.strip() for stmt in text.split(";") if stmt.strip()]


def copy_subset(con: duckdb.DuckDBPyConnection) -> None:
    for stmt in subset_statements():
        con.execute(stmt)


def summarize(con: duckdb.DuckDBPyConnection) -> None:
    print("=== TPC-H test database summary ===")
    for table in (
        "region",
        "nation",
        "customer",
        "supplier",
        "part",
        "partsupp",
        "orders",
        "lineitem",
    ):
        count = con.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        print(f"  {table}: {count:,} rows")


def build(source: Path, dest: Path, force: bool) -> None:
    if not source.exists():
        raise FileNotFoundError(f"source database not found: {source}")
    if dest.exists():
        if not force:
            print(f"Reusing existing test database: {dest}")
            con = duckdb.connect(str(dest), read_only=True)
            try:
                summarize(con)
            finally:
                con.close()
            return
        dest.unlink()

    dest.parent.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect(str(dest))
    try:
        con.execute(f"ATTACH '{source.as_posix()}' AS src (READ_ONLY)")
        copy_subset(con)
        summarize(con)
    finally:
        con.close()

    size_mb = dest.stat().st_size / (1024 * 1024)
    print(f"  database size: {size_mb:.1f} MB")
    print(f"  wrote: {dest}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a small TPC-H DuckDB for tests")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        build(args.source, args.dest, args.force)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
