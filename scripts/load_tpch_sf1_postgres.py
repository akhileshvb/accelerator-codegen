#!/usr/bin/env python3
"""Stream TPC-H tables from DuckDB (SF1+) into PostgreSQL without fetching all rows."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import duckdb
import psycopg2
from psycopg2.extras import execute_values

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DUCKDB = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb"

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
BATCH_SIZE = 50_000


def connect_postgres(dbname: str):
    host = os.environ.get("PG_HOST", os.environ.get("PGHOST", "127.0.0.1"))
    port = int(os.environ.get("PG_PORT", os.environ.get("PGPORT", "5434")))
    user = os.environ.get("PGUSER", "postgres")
    password = os.environ.get("PGPASSWORD", "postgres")
    return psycopg2.connect(
        host=host, port=port, user=user, password=password, dbname=dbname
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


def copy_table(
    duck: duckdb.DuckDBPyConnection,
    pg_cur,
    table: str,
) -> int:
    columns = table_columns(duck, table)
    column_sql = ", ".join(columns)
    placeholders = ", ".join(["%s"] * len(columns))
    pg_cur.execute(f"TRUNCATE {table} CASCADE")
    insert_sql = f"INSERT INTO {table} ({column_sql}) VALUES %s"
    result = duck.execute(f"SELECT {column_sql} FROM {table}")
    copied = 0
    while True:
        batch = result.fetchmany(BATCH_SIZE)
        if not batch:
            break
        execute_values(pg_cur, insert_sql, batch, template=f"({placeholders})")
        copied += len(batch)
        if copied % 500_000 == 0 or len(batch) < BATCH_SIZE:
            print(f"  {table}: {copied:,} rows", flush=True)
    return copied


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    parser.add_argument("--db", required=True)
    args = parser.parse_args()

    if not args.duckdb.exists():
        raise FileNotFoundError(f"DuckDB database not found: {args.duckdb}")

    duck = duckdb.connect(str(args.duckdb), read_only=True)
    pg = connect_postgres(args.db)
    pg.autocommit = False
    cur = pg.cursor()
    try:
        cur.execute("SET synchronous_commit = off")
        for table in TABLES:
            print(f"loading {table}...", flush=True)
            n = copy_table(duck, cur, table)
            pg.commit()
            print(f"  {table}: done ({n:,})", flush=True)
        print("analyzing...", flush=True)
        cur.execute("ANALYZE")
        pg.commit()
    finally:
        cur.close()
        pg.close()
        duck.close()
    print(f"SF1 load complete into {args.db}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
