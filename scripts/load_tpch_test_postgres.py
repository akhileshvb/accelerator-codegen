#!/usr/bin/env python3
"""Load TPC-H tables from a DuckDB test database into PostgreSQL."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import duckdb
import psycopg2
from psycopg2.extras import execute_values

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DUCKDB = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_test.duckdb"

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
BATCH_SIZE = 2000


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


def copy_table(
    duck: duckdb.DuckDBPyConnection,
    pg_cur,
    table: str,
) -> int:
    columns = table_columns(duck, table)
    column_sql = ", ".join(columns)
    placeholders = ", ".join(["%s"] * len(columns))
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    parser.add_argument("--db", required=True)
    args = parser.parse_args()

    if not args.duckdb.exists():
        raise FileNotFoundError(f"DuckDB test database not found: {args.duckdb}")

    duck = duckdb.connect(str(args.duckdb), read_only=True)
    pg = connect_postgres(args.db)
    try:
        with pg.cursor() as cur:
            for table in TABLES:
                count = copy_table(duck, cur, table)
                print(f"loaded {table}: {count} rows")
        pg.commit()
    finally:
        duck.close()
        pg.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
