#!/usr/bin/env python3
"""Create a small SEC EDGAR DuckDB for fast shared-interface tests.

Samples filings from the full benchmark database so every shared SEC query
has probe-compatible rows, while keeping the database small enough for CI.

Usage:
    python3 scripts/create_sec_edgar_test_db.py \\
        --source ../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb \\
        --dest ../accelerator-codegen-duckdb/sec_edgar_test.duckdb
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import duckdb

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT.parent / "accelerator-codegen-duckdb" / "sec_edgar_sf3.duckdb"
DEFAULT_DEST = ROOT.parent / "accelerator-codegen-duckdb" / "sec_edgar_test.duckdb"
SUBSET_SQL = ROOT / "workloads" / "sec_edgar" / "test_db_subset.sql"

PROBE_CHECKS = {
    "q1": "SELECT adsh, line, stmt, rfile FROM pre WHERE stmt = 'BS' ORDER BY adsh, line LIMIT 1",
    "q2": (
        "SELECT n.adsh, n.tag, n.value, n.uom FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
        "ORDER BY n.adsh, n.tag, n.value DESC LIMIT 1"
    ),
    "q3": (
        "SELECT n.adsh, n.value, n.uom FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL "
        "ORDER BY n.adsh LIMIT 1"
    ),
    # Q4 keeps only (sic, tlabel) groups with >= 2 companies, so its catalog
    # probe needs a row in such a group (the WHERE filter alone is not enough).
    "q4": (
        "SELECT n.adsh FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
        "AND n.value IS NOT NULL AND t.abstract = 0 "
        "AND (s.sic, t.tlabel) IN ("
        "  SELECT s2.sic, t2.tlabel FROM num n2 "
        "  JOIN sub s2 ON n2.adsh = s2.adsh "
        "  JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version "
        "  JOIN tag t2 ON n2.tag = t2.tag AND n2.version = t2.version "
        "  WHERE n2.uom = 'USD' AND p2.stmt = 'EQ' AND s2.sic BETWEEN 4000 AND 4999 "
        "  AND n2.value IS NOT NULL AND t2.abstract = 0 "
        "  GROUP BY s2.sic, t2.tlabel HAVING COUNT(DISTINCT s2.cik) >= 2) "
        "LIMIT 1"
    ),
    "q6": (
        "SELECT n.adsh, n.tag, n.version, n.value, n.uom FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
        "AND n.value IS NOT NULL "
        "ORDER BY n.adsh, n.tag, n.version LIMIT 1"
    ),
    "q24": (
        "SELECT n.rowid, n.adsh, n.tag, n.version, n.uom, n.value, n.ddate FROM num n "
        "LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh "
        "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
        "AND n.value IS NOT NULL AND p.adsh IS NULL LIMIT 1"
    ),
}


def subset_statements() -> list[str]:
    """The statements of workloads/sec_edgar/test_db_subset.sql (shared with
    ctest's make_sec_edgar_test_db, so both build the same subset)."""
    text = re.sub(r"--[^\n]*", "", SUBSET_SQL.read_text())
    return [stmt.strip() for stmt in text.split(";") if stmt.strip()]


def copy_subset(con: duckdb.DuckDBPyConnection) -> None:
    for stmt in subset_statements():
        con.execute(stmt)


def summarize(con: duckdb.DuckDBPyConnection) -> None:
    print("=== SEC EDGAR test database summary ===")
    for table in ("sub", "num", "tag", "pre"):
        count = con.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        print(f"  {table}: {count:,} rows")
    for name, sql in PROBE_CHECKS.items():
        count = con.execute(f"SELECT COUNT(*) FROM ({sql}) probes").fetchone()[0]
        print(f"  probe {name}: {count} row(s)")


def verify_probes(con: duckdb.DuckDBPyConnection) -> None:
    missing = [name for name, sql in PROBE_CHECKS.items() if con.execute(sql).fetchone() is None]
    if missing:
        raise RuntimeError(f"test database missing probe rows for: {', '.join(missing)}")


def build(source: Path, dest: Path, force: bool) -> None:
    if not source.exists():
        raise FileNotFoundError(f"source database not found: {source}")
    if dest.exists():
        if not force:
            con = duckdb.connect(str(dest), read_only=True)
            try:
                verify_probes(con)
            finally:
                con.close()
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
        verify_probes(con)
        summarize(con)
    finally:
        con.close()

    size_mb = dest.stat().st_size / (1024 * 1024)
    print(f"  database size: {size_mb:.1f} MB")
    print(f"  wrote: {dest}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a small SEC EDGAR DuckDB for tests")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--force", action="store_true", help="Rebuild even if dest exists")
    args = parser.parse_args()

    try:
        build(args.source, args.dest, args.force)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
