#!/usr/bin/env python3
"""Load TPC-H tables from DuckDB into PostgreSQL via per-table CSV staging.

Streams one table at a time (DuckDB COPY → temp CSV → psql \\copy) so SF20
fits without holding full tables in RAM. Temp CSV is deleted after each table.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import duckdb

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DUCKDB = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf20.duckdb"

TABLES = (
    "region",
    "nation",
    "customer",
    "supplier",
    "part",
    "partsupp",
    "orders",
    "lineitem",
)


def table_columns(con: duckdb.DuckDBPyConnection, table: str) -> list[str]:
    rows = con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = ?
        ORDER BY ordinal_position
        """,
        [table],
    ).fetchall()
    if not rows:
        raise RuntimeError(f"table not found in DuckDB source: {table}")
    return [row[0] for row in rows]


def psql_env() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("PGHOST", env.get("PG_HOST", "127.0.0.1"))
    env.setdefault("PGPORT", env.get("PG_PORT", env.get("PGPORT", "5434")))
    env.setdefault("PGUSER", env.get("PG_USER", "postgres"))
    env.setdefault("PGPASSWORD", env.get("PGPASSWORD", "postgres"))
    return env


def run_psql(db: str, *args: str) -> None:
    cmd = ["psql", "-v", "ON_ERROR_STOP=1", "-d", db, *args]
    subprocess.run(cmd, check=True, env=psql_env())


def copy_table(
    duck: duckdb.DuckDBPyConnection,
    db: str,
    table: str,
    staging_dir: Path,
) -> int:
    columns = table_columns(duck, table)
    column_sql = ", ".join(columns)
    csv_path = staging_dir / f"{table}.csv"

    count = int(duck.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
    print(f"exporting {table} ({count:,} rows) → {csv_path}", flush=True)
    t0 = time.time()
    # Quote path for DuckDB SQL.
    out = str(csv_path).replace("'", "''")
    duck.execute(
        f"COPY (SELECT {column_sql} FROM {table}) TO '{out}' "
        f"(HEADER false, DELIMITER ',', QUOTE '\"', ESCAPE '\"')"
    )
    print(f"  exported in {time.time() - t0:.1f}s", flush=True)

    run_psql(db, "-c", f"TRUNCATE {table}")
    copy_sql = (
        f"\\copy {table} ({column_sql}) FROM '{csv_path}' "
        f"WITH (FORMAT csv, DELIMITER ',', QUOTE '\"', ESCAPE '\"')"
    )
    print(f"importing {table} into postgres...", flush=True)
    t1 = time.time()
    run_psql(db, "-c", copy_sql)
    print(f"  imported in {time.time() - t1:.1f}s", flush=True)

    csv_path.unlink(missing_ok=True)
    return count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    parser.add_argument("--db", required=True)
    parser.add_argument("--staging-dir", type=Path, default=None)
    parser.add_argument(
        "--tables",
        default=",".join(TABLES),
        help="Comma-separated table list (default: all TPC-H tables)",
    )
    args = parser.parse_args()

    if not args.duckdb.exists():
        raise FileNotFoundError(f"DuckDB database not found: {args.duckdb}")

    tables = [t.strip() for t in args.tables.split(",") if t.strip()]
    unknown = [t for t in tables if t not in TABLES]
    if unknown:
        raise SystemExit(f"unknown tables: {unknown}; allowed={list(TABLES)}")

    staging = args.staging_dir
    tmp: tempfile.TemporaryDirectory[str] | None = None
    if staging is None:
        tmp = tempfile.TemporaryDirectory(prefix="tpch_pg_load_")
        staging = Path(tmp.name)
    staging.mkdir(parents=True, exist_ok=True)

    print(f"source={args.duckdb}")
    print(f"dest_db={args.db}")
    print(f"staging={staging}")

    duck = duckdb.connect(str(args.duckdb), read_only=True)
    try:
        run_psql(args.db, "-c", "SET synchronous_commit = off")
        for table in tables:
            t0 = time.time()
            count = copy_table(duck, args.db, table, staging)
            print(
                f"loaded {table}: {count:,} rows in {time.time() - t0:.1f}s",
                flush=True,
            )
    finally:
        duck.close()
        if tmp is not None:
            tmp.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
