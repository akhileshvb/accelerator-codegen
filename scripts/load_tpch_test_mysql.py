#!/usr/bin/env python3
"""Load TPC-H tables from a DuckDB test database into MySQL."""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
from pathlib import Path

import duckdb

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


def mysql_cmd(db: str) -> list[str]:
    container = os.environ.get("ACCEL_MYSQL_CONTAINER", "accel_mysql_test")
    password = os.environ.get("MYSQL_PWD", os.environ.get("MYSQL_ROOT_PASSWORD", "mysql"))
    if os.environ.get("ACCEL_MYSQL_DOCKER", "1") == "1":
        return [
            "docker",
            "exec",
            "-i",
            container,
            "mysql",
            "-h",
            "127.0.0.1",
            "-P",
            "3306",
            "-uroot",
            f"-p{password}",
            db,
            "--local-infile=1",
            "--batch",
        ]
    return [
        "mysql",
        "-h",
        os.environ.get("MYSQL_HOST", "127.0.0.1"),
        "-P",
        os.environ.get("MYSQL_PORT", "3307"),
        "-uroot",
        f"-p{password}",
        db,
        "--local-infile=1",
        "--batch",
    ]


def run_sql(db: str, sql: str) -> None:
    proc = subprocess.run(
        mysql_cmd(db),
        input=sql.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"mysql failed ({proc.returncode}): {proc.stderr.decode()}\nSQL:\n{sql[:500]}"
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


def copy_table(duck: duckdb.DuckDBPyConnection, db: str, table: str, tmp: Path) -> int:
    columns = table_columns(duck, table)
    rows = duck.execute(f"SELECT {', '.join(columns)} FROM {table}").fetchall()
    tsv = tmp / f"{table}.tsv"
    with tsv.open("w", newline="") as fh:
        writer = csv.writer(fh, delimiter="\t", lineterminator="\n", quoting=csv.QUOTE_MINIMAL)
        for row in rows:
            writer.writerow(["" if v is None else v for v in row])
    col_sql = ", ".join(columns)
    infile = f"/accel-src/tmp/mysql_tpch/{table}.tsv"
    run_sql(db, f"TRUNCATE TABLE {table};")
    run_sql(
        db,
        "LOAD DATA LOCAL INFILE '"
        + infile
        + f"' INTO TABLE {table} FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' "
        + f"({col_sql});",
    )
    return len(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=DEFAULT_DUCKDB)
    parser.add_argument("--db", required=True)
    args = parser.parse_args()
    if not args.duckdb.exists():
        raise FileNotFoundError(f"DuckDB test database not found: {args.duckdb}")
    tmp = ROOT / "tmp" / "mysql_tpch"
    tmp.mkdir(parents=True, exist_ok=True)
    duck = duckdb.connect(str(args.duckdb), read_only=True)
    try:
        for table in TABLES:
            count = copy_table(duck, args.db, table, tmp)
            print(f"loaded {table}: {count} rows")
    finally:
        duck.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
