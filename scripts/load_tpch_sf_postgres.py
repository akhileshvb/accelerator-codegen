#!/usr/bin/env python3
"""Load TPC-H tables from a DuckDB SF database into PostgreSQL via COPY."""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

import duckdb
import psycopg2

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


def connect_postgres(dbname: str):
    host = os.environ.get("PG_HOST", "127.0.0.1")
    port = int(os.environ.get("PG_PORT", "5434"))
    return psycopg2.connect(
        host=host, port=port, user="postgres", password=os.environ.get("PGPASSWORD", "postgres"),
        dbname=dbname,
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
    pg_conn,
    table: str,
    staging_dir: Path,
) -> int:
    columns = table_columns(duck, table)
    column_sql = ", ".join(columns)
    row_count = int(duck.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
    with pg_conn.cursor() as cur:
        cur.execute(f"TRUNCATE {table} CASCADE")
    pg_conn.commit()
    if row_count == 0:
        return 0

    staging_dir.mkdir(parents=True, exist_ok=True)
    export_path = staging_dir / f"{table}.csv"
    duck.execute(
        f"COPY (SELECT {column_sql} FROM {table}) TO '{export_path}' "
        "(HEADER, DELIMITER E'\\t', NULL '')"
    )
    with pg_conn.cursor() as cur:
        with open(export_path, "rb") as source:
            cur.copy_expert(
                f"COPY {table} ({column_sql}) FROM STDIN WITH "
                "(FORMAT csv, HEADER true, DELIMITER E'\\t', NULL '')",
                source,
            )
    pg_conn.commit()
    try:
        export_path.unlink()
    except OSError:
        pass
    return row_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    parser.add_argument("--db", required=True)
    parser.add_argument(
        "--staging-dir",
        type=Path,
        default=Path(os.environ.get("ACCEL_PG_STAGING", "/disk3/akhilvb/tpch_sf20_pg_staging")),
    )
    args = parser.parse_args()

    if not args.duckdb.exists():
        raise FileNotFoundError(f"DuckDB source not found: {args.duckdb}")

    duck = duckdb.connect(str(args.duckdb), read_only=True)
    pg = connect_postgres(args.db)
    try:
        # Disable triggers during bulk load for speed; re-enable after.
        with pg.cursor() as cur:
            cur.execute("SET session_replication_role = replica")
        for table in TABLES:
            count = copy_table(duck, pg, table, args.staging_dir)
            print(f"loaded {table}: {count:,} rows", flush=True)
        with pg.cursor() as cur:
            cur.execute("SET session_replication_role = DEFAULT")
        pg.commit()
    finally:
        duck.close()
        pg.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
