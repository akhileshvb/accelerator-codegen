#!/usr/bin/env python3
"""Load SEC EDGAR tables from a DuckDB test database into PostgreSQL."""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

import duckdb
import psycopg2
from psycopg2.extras import execute_values

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DUCKDB = ROOT.parent / "accelerator-codegen-duckdb" / "sec_edgar_test.duckdb"

TABLES = ("sub", "num", "pre", "tag")
BATCH_SIZE = 2000
COPY_THRESHOLD = 100_000


def connect_postgres(dbname: str):
    host = os.environ.get("PG_HOST", "127.0.0.1")
    port = int(os.environ.get("PG_PORT", "5434"))
    return psycopg2.connect(host=host, port=port, user="postgres", dbname=dbname)


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


def copy_table_insert(
    duck: duckdb.DuckDBPyConnection,
    pg_cur,
    table: str,
    *,
    truncate: bool,
) -> int:
    columns = table_columns(duck, table)
    column_sql = ", ".join(columns)
    placeholders = ", ".join(["%s"] * len(columns))
    if truncate:
        pg_cur.execute(f"TRUNCATE {table}")
    rows = duck.execute(f"SELECT {column_sql} FROM {table}").fetchall()
    if not rows:
        return 0
    insert_sql = f"INSERT INTO {table} ({column_sql}) VALUES %s"
    copied = 0
    for start in range(0, len(rows), BATCH_SIZE):
        batch = rows[start : start + BATCH_SIZE]
        execute_values(pg_cur, insert_sql, batch, template=f"({placeholders})")
        copied += len(batch)
    return copied


def copy_table_bulk(
    duck: duckdb.DuckDBPyConnection,
    pg_conn,
    table: str,
    *,
    truncate: bool,
) -> int:
    columns = table_columns(duck, table)
    column_sql = ", ".join(columns)
    row_count = duck.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
    if truncate:
        with pg_conn.cursor() as pg_cur:
            pg_cur.execute(f"TRUNCATE {table}")
        pg_conn.commit()
    if row_count == 0:
        return 0

    with tempfile.NamedTemporaryFile(mode="w+b", suffix=f"_{table}.csv") as handle:
        export_path = handle.name
        duck.execute(
            f"COPY (SELECT {column_sql} FROM {table}) TO '{export_path}' "
            "(HEADER, DELIMITER E'\\t', NULL '')"
        )
        with pg_conn.cursor() as pg_cur:
            with open(export_path, "rb") as source:
                pg_cur.copy_expert(
                    f"COPY {table} ({column_sql}) FROM STDIN WITH "
                    "(FORMAT csv, HEADER true, DELIMITER E'\\t', NULL '')",
                    source,
                )
        pg_conn.commit()
    return row_count


def copy_table(
    duck: duckdb.DuckDBPyConnection,
    pg_conn,
    table: str,
    *,
    truncate: bool,
    method: str,
) -> int:
    row_count = duck.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
    if method == "auto":
        method = "copy" if row_count >= COPY_THRESHOLD else "insert"
    if method == "copy":
        return copy_table_bulk(duck, pg_conn, table, truncate=truncate)
    with pg_conn.cursor() as pg_cur:
        return copy_table_insert(duck, pg_cur, table, truncate=truncate)


def load_database(
    duckdb_path: Path,
    pg_db: str,
    *,
    truncate: bool,
    method: str,
) -> None:
    if not duckdb_path.exists():
        raise FileNotFoundError(f"DuckDB source not found: {duckdb_path}")

    duck = duckdb.connect(str(duckdb_path), read_only=True)
    pg = connect_postgres(pg_db)
    try:
        for table in TABLES:
            count = copy_table(duck, pg, table, truncate=truncate, method=method)
            print(f"  loaded {table}: {count:,} rows")
    finally:
        duck.close()
        pg.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Load SEC EDGAR test data into PostgreSQL")
    parser.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    parser.add_argument("--db", default="accel_sec_edgar_test")
    parser.add_argument("--truncate", action="store_true")
    parser.add_argument(
        "--method",
        choices=("auto", "insert", "copy"),
        default="auto",
        help="load strategy (auto uses COPY for large tables)",
    )
    args = parser.parse_args()

    try:
        load_database(args.duckdb, args.db, truncate=args.truncate, method=args.method)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
