#!/usr/bin/env python3
"""TPC-H RF1/RF2 mini-chunk writes for PostgreSQL (mirrors DuckDB tpch_rf.cpp).

Uses bulk INSERT/DELETE so statement-level mutation triggers fire once per table
per chunk (required for 0.1% SF20 RF sets of ~30k orders).
"""

from __future__ import annotations

from typing import List, Sequence


ORD_INSERT_COLS = (
    "o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, "
    "o_orderpriority, o_clerk, o_shippriority, o_comment"
)
LI_INSERT_COLS = (
    "l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, "
    "l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, "
    "l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, "
    "l_comment"
)


def _orders_n(cur) -> int:
    cur.execute(
        "SELECT COALESCE(reltuples, 0)::bigint FROM pg_class WHERE oid = 'orders'::regclass"
    )
    n = int(cur.fetchone()[0])
    if n > 0:
        return n
    cur.execute("SELECT COUNT(*)::bigint FROM orders")
    return int(cur.fetchone()[0])


def pick_delete_orderkeys(cur, order_count: int, write_seq: int) -> List[int]:
    n = _orders_n(cur)
    if n <= 0:
        raise RuntimeError("RF2: orders table empty")
    take = min(max(1, order_count), n)
    offset = ((write_seq // 2) * take) % n
    cur.execute(
        "SELECT o_orderkey FROM orders ORDER BY o_orderkey "
        f"LIMIT {take} OFFSET {offset}"
    )
    return [int(r[0]) for r in cur.fetchall()]


def apply_rf2_delete(cur, orderkeys: Sequence[int]) -> None:
    if not orderkeys:
        raise RuntimeError("RF2 requires orderkeys")
    cur.execute("DROP TABLE IF EXISTS _accel_rf_del")
    cur.execute(
        "CREATE TEMP TABLE _accel_rf_del (o_orderkey bigint PRIMARY KEY) ON COMMIT DROP"
    )
    cur.execute(
        "INSERT INTO _accel_rf_del (o_orderkey) VALUES "
        + ",".join("(%s)" % int(k) for k in orderkeys)
    )
    cur.execute(
        f"DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM _accel_rf_del)"
    )
    cur.execute(
        f"DELETE FROM orders WHERE o_orderkey IN (SELECT o_orderkey FROM _accel_rf_del)"
    )


def apply_rf2_delete_bulk(cur, order_count: int, write_seq: int) -> None:
    n = _orders_n(cur)
    if n <= 0:
        raise RuntimeError("RF2: orders table empty")
    take = min(max(1, order_count), n)
    offset = ((write_seq // 2) * take) % n
    cur.execute("DROP TABLE IF EXISTS _accel_rf_del")
    cur.execute(
        "CREATE TEMP TABLE _accel_rf_del ON COMMIT DROP AS "
        "SELECT o_orderkey FROM orders ORDER BY o_orderkey "
        f"LIMIT {take} OFFSET {offset}"
    )
    cur.execute("DELETE FROM lineitem WHERE l_orderkey IN (SELECT o_orderkey FROM _accel_rf_del)")
    cur.execute("DELETE FROM orders WHERE o_orderkey IN (SELECT o_orderkey FROM _accel_rf_del)")


def apply_rf1_insert(cur, order_count: int) -> List[int]:
    take = max(1, order_count)
    cur.execute("SELECT COALESCE(MAX(o_orderkey), 0)::bigint FROM orders")
    next_key = int(cur.fetchone()[0]) + 1
    cur.execute("DROP TABLE IF EXISTS _accel_rf_ord")
    cur.execute(
        "CREATE TEMP TABLE _accel_rf_ord ON COMMIT DROP AS "
        "SELECT "
        f"{next_key} - 1 + row_number() OVER (ORDER BY o_orderkey) AS new_key, "
        "s.o_orderkey AS old_key, s.o_custkey, s.o_orderstatus, s.o_totalprice, "
        "s.o_orderdate, s.o_orderpriority, s.o_clerk, s.o_shippriority, s.o_comment "
        "FROM ("
        "SELECT o_orderkey, o_custkey, o_orderstatus, o_totalprice, o_orderdate, "
        "o_orderpriority, o_clerk, o_shippriority, o_comment "
        f"FROM orders ORDER BY o_orderkey LIMIT {take}"
        ") s"
    )
    cur.execute(
        f"INSERT INTO orders ({ORD_INSERT_COLS}) "
        "SELECT new_key, o_custkey, o_orderstatus, o_totalprice, o_orderdate, "
        "o_orderpriority, o_clerk, o_shippriority, o_comment FROM _accel_rf_ord"
    )
    cur.execute(
        f"INSERT INTO lineitem ({LI_INSERT_COLS}) "
        "SELECT o.new_key, l.l_partkey, l.l_suppkey, l.l_linenumber, l.l_quantity, "
        "l.l_extendedprice, l.l_discount, l.l_tax, l.l_returnflag, l.l_linestatus, "
        "l.l_shipdate, l.l_commitdate, l.l_receiptdate, l.l_shipinstruct, "
        "l.l_shipmode, l.l_comment "
        "FROM _accel_rf_ord o JOIN lineitem l ON l.l_orderkey = o.old_key"
    )
    cur.execute("SELECT new_key FROM _accel_rf_ord ORDER BY new_key")
    return [int(r[0]) for r in cur.fetchall()]


def apply_rf_chunk(cur, write_seq: int, order_count: int = 1) -> str:
    """Even seq => RF2 delete; odd => RF1 insert. Returns kind name."""
    if (write_seq % 2) == 0:
        apply_rf2_delete_bulk(cur, order_count, write_seq)
        return "rf2"
    apply_rf1_insert(cur, order_count)
    return "rf1"
