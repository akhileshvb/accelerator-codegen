#!/usr/bin/env python3
"""Create a small TPC-H DuckDB for fast shared-interface tests."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import duckdb

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb"
DEFAULT_DEST = ROOT.parent / "accelerator-codegen-duckdb" / "tpch_test.duckdb"

PROBE_SEEDS = """
CREATE TEMP TABLE seed_lineitems AS
SELECT DISTINCT l_orderkey, l_linenumber FROM (
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
     LIMIT 120)
    UNION
    (SELECT l.l_orderkey, l.l_linenumber
     FROM src.lineitem l
     JOIN src.orders o ON l.l_orderkey = o.o_orderkey
     JOIN src.customer c ON o.o_custkey = c.c_custkey
     WHERE c.c_mktsegment = 'BUILDING'
       AND o.o_orderdate < DATE '1995-03-15'
       AND l.l_shipdate > DATE '1995-03-15'
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipdate >= DATE '1995-01-01'
       AND l_shipdate < DATE '1996-01-01'
       AND l_discount BETWEEN 0.02 AND 0.04
       AND l_quantity < 25
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_returnflag = 'R'
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipmode IN ('MAIL', 'SHIP')
       AND l_receiptdate >= DATE '1995-01-01'
       AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year
     LIMIT 120)
    UNION
    (SELECT l_orderkey, l_linenumber
     FROM src.lineitem
     WHERE l_shipdate >= DATE '1995-06-01'
       AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month
     LIMIT 120)
    UNION
    (SELECT l.l_orderkey, l.l_linenumber FROM src.lineitem l
     JOIN src.part p ON l.l_partkey = p.p_partkey
     WHERE p.p_size = 10 AND p.p_type LIKE '%BRASS%' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem l
     JOIN src.orders o ON l.l_orderkey = o.o_orderkey
     WHERE o.o_orderdate >= DATE '1996-01-01'
       AND o.o_orderdate < DATE '1997-01-01' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem
     WHERE l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem
     WHERE l_shipdate >= DATE '1994-11-01'
       AND l_shipdate < DATE '1995-02-01' LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem
     WHERE l_quantity > 300 LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem l
     JOIN src.part p ON l.l_partkey = p.p_partkey
     WHERE p.p_brand IN ('Brand#31', 'Brand#12', 'Brand#51') LIMIT 80)
    UNION
    (SELECT l_orderkey, l_linenumber FROM src.lineitem l
     JOIN src.orders o ON l.l_orderkey = o.o_orderkey
     WHERE o.o_orderstatus = 'F' LIMIT 80)
) seeds
"""


def copy_subset(con: duckdb.DuckDBPyConnection) -> None:
    con.execute(PROBE_SEEDS)
    con.execute(
        """
        CREATE TABLE lineitem AS
        SELECT *
        FROM src.lineitem
        WHERE l_orderkey IN (SELECT DISTINCT l_orderkey FROM seed_lineitems)
        """
    )
    con.execute(
        """
        CREATE TABLE orders AS
        SELECT *
        FROM src.orders
        WHERE o_orderkey IN (SELECT DISTINCT l_orderkey FROM lineitem)
        """
    )
    con.execute(
        """
        CREATE TABLE customer AS
        SELECT *
        FROM src.customer
        WHERE c_custkey IN (SELECT DISTINCT o_custkey FROM orders)
        """
    )
    con.execute(
        """
        CREATE TABLE supplier AS
        SELECT *
        FROM src.supplier
        WHERE s_suppkey IN (SELECT DISTINCT l_suppkey FROM lineitem)
        """
    )
    con.execute(
        """
        CREATE TABLE nation AS
        SELECT *
        FROM src.nation
        WHERE n_nationkey IN (
            SELECT DISTINCT c_nationkey FROM customer
            UNION
            SELECT DISTINCT s_nationkey FROM supplier
        )
        """
    )
    con.execute(
        """
        CREATE TABLE region AS
        SELECT *
        FROM src.region
        WHERE r_regionkey IN (SELECT DISTINCT n_regionkey FROM nation)
        """
    )
    con.execute(
        """
        CREATE TABLE part AS
        SELECT *
        FROM src.part
        WHERE p_partkey IN (SELECT DISTINCT l_partkey FROM lineitem)
        """
    )
    con.execute(
        """
        CREATE TABLE partsupp AS
        SELECT *
        FROM src.partsupp
        WHERE (ps_partkey, ps_suppkey) IN (
            SELECT l_partkey, l_suppkey FROM lineitem
        )
        """
    )


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
