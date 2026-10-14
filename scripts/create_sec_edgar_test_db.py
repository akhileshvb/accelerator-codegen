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
SCHEMA_PATH = ROOT / "eval" / "bench" / "sec_edgar" / "schema.sql"

PROBE_CHECKS = {
    "q1": "SELECT adsh, line, stmt, rfile FROM pre WHERE stmt IS NOT NULL ORDER BY adsh, line LIMIT 1",
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
    "q4": (
        "SELECT n.adsh, n.tag, n.version, n.value, n.uom FROM num n "
        "JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
        "AND n.value IS NOT NULL AND t.abstract = 0 "
        "ORDER BY n.adsh, n.tag, n.version LIMIT 1"
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


def load_schema_statements() -> list[str]:
    schema_sql = SCHEMA_PATH.read_text()
    schema_clean = re.sub(r"--[^\n]*", "", schema_sql)
    schema_clean = re.sub(
        r",?\s*FOREIGN KEY\s*\([^)]+\)\s*REFERENCES\s+\w+\([^)]+\)",
        "",
        schema_clean,
    )
    schema_clean = re.sub(r"\s*REFERENCES\s+\w+\([^)]+\)", "", schema_clean)
    schema_clean = re.sub(r",\s*\)", "\n)", schema_clean)
    return [stmt.strip() for stmt in schema_clean.split(";") if stmt.strip()]


def seed_sql(per_seed_limit: int) -> str:
    lim = per_seed_limit
    return f"""
    CREATE TEMP TABLE selected_adsh AS
    SELECT DISTINCT adsh FROM (
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL
         LIMIT {lim})
        UNION
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL
         LIMIT {lim})
        UNION
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
         JOIN src.tag t ON n.tag = t.tag AND n.version = t.version
         WHERE n.uom = 'USD' AND p.stmt = 'EQ'
           AND s.sic BETWEEN 4000 AND 4999
           AND n.value IS NOT NULL AND t.abstract = 0
         LIMIT {lim})
        UNION
        (SELECT n.adsh
         FROM src.num n
         JOIN src.sub s ON n.adsh = s.adsh
         JOIN src.pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
         WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL
         LIMIT {lim})
        UNION
        (SELECT adsh
         FROM src.pre
         WHERE stmt IS NOT NULL
         LIMIT {lim})
        UNION
        (SELECT n.adsh
         FROM src.num n
         LEFT JOIN src.pre p
           ON n.tag = p.tag AND n.version = p.version AND n.adsh = p.adsh
         WHERE n.uom = 'USD'
           AND n.ddate BETWEEN 20230101 AND 20231231
           AND n.value IS NOT NULL
           AND p.adsh IS NULL
         LIMIT {lim})
    ) seeds
    """


def create_tables(con: duckdb.DuckDBPyConnection) -> None:
    for stmt in load_schema_statements():
        if stmt.upper().startswith("CREATE"):
            con.execute(stmt)


def copy_subset(con: duckdb.DuckDBPyConnection, per_seed_limit: int) -> None:
    con.execute(seed_sql(per_seed_limit))
    con.execute("CREATE TABLE sub AS SELECT * FROM src.sub WHERE adsh IN (SELECT adsh FROM selected_adsh)")
    con.execute("CREATE TABLE num AS SELECT * FROM src.num WHERE adsh IN (SELECT adsh FROM selected_adsh)")
    con.execute("CREATE TABLE pre AS SELECT * FROM src.pre WHERE adsh IN (SELECT adsh FROM selected_adsh)")
    con.execute(
        """
        CREATE TABLE tag AS
        SELECT DISTINCT t.*
        FROM src.tag t
        JOIN (
            SELECT DISTINCT tag, version FROM num
            UNION
            SELECT DISTINCT tag, version FROM pre
        ) refs ON t.tag = refs.tag AND t.version = refs.version
        """
    )


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


def build(source: Path, dest: Path, per_seed_limit: int, force: bool) -> None:
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
        copy_subset(con, per_seed_limit)
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
    parser.add_argument(
        "--per-seed-limit",
        type=int,
        default=80,
        help="Max filings sampled per query seed (default: 80)",
    )
    parser.add_argument("--force", action="store_true", help="Rebuild even if dest exists")
    args = parser.parse_args()

    try:
        build(args.source, args.dest, args.per_seed_limit, args.force)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
