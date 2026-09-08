"""Shared-interface query catalog for PostgreSQL benchmarks."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple


@dataclass(frozen=True)
class SharedQueryEntry:
    query_id: int
    workload: str
    base_sql: str
    aux_sql: str
    kernel_sql: str
    probe_select_sql: str
    mutation_table: int
    compare_unordered: bool = False
    build_probe: Optional[Callable[[Tuple], dict]] = None


def _tpch_q1_probe(row):
    orderkey, linenumber, *_rest, price = row[0], row[1], *row[2:-1], row[4]
    return {
        "update": (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_extendedprice = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (price,),
    }


def _lineitem_price_probe(row):
    orderkey, linenumber, price = row[0], row[1], row[2]
    return {
        "update": (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 0.01 "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_extendedprice = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (price,),
    }


def _lineitem_quantity_probe(row):
    orderkey, linenumber, quantity = row[0], row[1], row[2]
    return {
        "update": (
            "UPDATE lineitem SET l_quantity = l_quantity + 0.01 "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_quantity = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (quantity,),
    }


def _lineitem_receipt_probe(row):
    orderkey, linenumber, receipt = row[0], row[1], row[4]
    return {
        "update": (
            "UPDATE lineitem SET l_receiptdate = l_receiptdate + INTERVAL '1' day "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_receiptdate = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (receipt,),
    }


def _customer_acctbal_probe(row):
    custkey, acctbal = row[0], row[1]
    return {
        "update": (
            f"UPDATE customer SET c_acctbal = c_acctbal + 0.01 "
            f"WHERE c_custkey = {custkey}"
        ),
        "restore": (
            f"UPDATE customer SET c_acctbal = %s WHERE c_custkey = {custkey}"
        ),
        "restore_args": (acctbal,),
    }


def _q4_unlate_probe(row):
    orderkey, linenumber, _commit, receipt = row[0], row[1], row[2], row[3]
    return {
        "update": (
            "UPDATE lineitem SET l_receiptdate = l_commitdate "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_receiptdate = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (receipt,),
    }


def _orders_comment_probe(row):
    orderkey, comment = row[0], row[2]
    return {
        "update": (
            "UPDATE orders SET o_comment = o_comment || ' unusual accounts' "
            f"WHERE o_orderkey = {orderkey}"
        ),
        "restore": (
            f"UPDATE orders SET o_comment = %s WHERE o_orderkey = {orderkey}"
        ),
        "restore_args": (comment,),
    }


def _partsupp_cost_large_probe(row):
    partkey, suppkey, cost = row[0], row[1], row[2]
    return {
        "update": (
            "UPDATE partsupp SET ps_supplycost = ps_supplycost + 1000.0 "
            f"WHERE ps_partkey = {partkey} AND ps_suppkey = {suppkey}"
        ),
        "restore": (
            "UPDATE partsupp SET ps_supplycost = %s "
            f"WHERE ps_partkey = {partkey} AND ps_suppkey = {suppkey}"
        ),
        "restore_args": (cost,),
    }


def _partsupp_suppkey_probe(row):
    partkey, suppkey = row[0], row[1]
    return {
        "update": (
            "UPDATE partsupp SET ps_suppkey = ps_suppkey + 1 "
            f"WHERE ps_partkey = {partkey} AND ps_suppkey = {suppkey}"
        ),
        "restore": (
            "UPDATE partsupp SET ps_suppkey = %s "
            f"WHERE ps_partkey = {partkey} AND ps_suppkey = {int(suppkey) + 1}"
        ),
        "restore_args": (suppkey,),
    }


def _part_brand_new_group_probe(row):
    partkey, brand = row[0], row[1]
    return {
        "update": (
            "UPDATE part SET p_brand = 'Brand#00' "
            f"WHERE p_partkey = {partkey}"
        ),
        "restore": (
            "UPDATE part SET p_brand = %s "
            f"WHERE p_partkey = {partkey}"
        ),
        "restore_args": (brand,),
    }


def _lineitem_price_large_probe(row):
    orderkey, linenumber, price = row[1], row[5], row[3]
    return {
        "update": (
            "UPDATE lineitem SET l_extendedprice = l_extendedprice + 100.00 "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_extendedprice = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (price,),
    }


def _lineitem_quantity_large_probe(row):
    orderkey, linenumber, quantity = row[0], row[2], row[3]
    return {
        "update": (
            "UPDATE lineitem SET l_quantity = l_quantity + 100 "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_quantity = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (quantity,),
    }


def _lineitem_quantity_huge_probe(row):
    orderkey, linenumber, quantity = row[0], row[1], row[2]
    return {
        "update": (
            "UPDATE lineitem SET l_quantity = l_quantity + 100000 "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_quantity = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (quantity,),
    }


def _lineitem_unlate_probe(row):
    """Flip a late lineitem to on-time so Q21-style waiting counts change."""
    orderkey, linenumber, _suppkey, _commit, receipt = row[0], row[1], row[2], row[3], row[4]
    return {
        "update": (
            "UPDATE lineitem SET l_receiptdate = l_commitdate "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore": (
            "UPDATE lineitem SET l_receiptdate = %s "
            f"WHERE l_orderkey = {orderkey} AND l_linenumber = {linenumber}"
        ),
        "restore_args": (receipt,),
    }


def _partsupp_cost_probe(row):
    partkey, suppkey, _availqty, supplycost = row[0], row[1], row[2], row[3]
    return {
        "update": (
            "UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01 "
            f"WHERE ps_partkey = {partkey} AND ps_suppkey = {suppkey}"
        ),
        "restore": (
            "UPDATE partsupp SET ps_supplycost = %s "
            f"WHERE ps_partkey = {partkey} AND ps_suppkey = {suppkey}"
        ),
        "restore_args": (supplycost,),
    }


def _sec_num_delta(value) -> float:
    magnitude = abs(float(value))
    return max(1.0, magnitude * 0.01)


def _sec_num_probe(row):
    adsh, tag, version, original, *_ = row[0], row[1], row[2], row[3], *row[4:]
    # Support both legacy compact rows and full num layouts ending in value.
    if len(row) >= 9:
        adsh, tag, version, original = row[1], row[2], row[3], row[8]
    delta = _sec_num_delta(original)
    return {
        "update": (
            f"UPDATE num SET value = value + {delta} "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND version = '{version}'"
        ),
        "restore": (
            "UPDATE num SET value = %s "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND version = '{version}'"
        ),
        "restore_args": (original,),
    }


def _sec_q1_probe(row):
    adsh, line, *_ = row
    return {
        "update": f"UPDATE pre SET line = line + 1 WHERE adsh = '{adsh}' AND line = {line}",
        "restore": (
            f"UPDATE pre SET line = %s WHERE adsh = '{adsh}' AND line = {int(line) + 1}"
        ),
        "restore_args": (line,),
    }


def _sec_q2_probe(row):
    if len(row) >= 9:
        adsh, tag, original = row[1], row[2], row[8]
    else:
        adsh, tag, original = row[0], row[1], row[2]
    delta = _sec_num_delta(original)
    return {
        "update": (
            f"UPDATE num SET value = value + {delta} "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND uom = 'pure'"
        ),
        "restore": (
            "UPDATE num SET value = %s "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND uom = 'pure'"
        ),
        "restore_args": (original,),
    }


def _sec_q3_probe(row):
    if len(row) >= 9:
        adsh, original = row[1], row[8]
    else:
        adsh, original = row[0], row[1]
    delta = _sec_num_delta(original)
    return {
        "update": (
            f"UPDATE num SET value = value + {delta} "
            f"WHERE adsh = '{adsh}' AND uom = 'USD'"
        ),
        "restore": (
            "UPDATE num SET value = %s "
            f"WHERE adsh = '{adsh}' AND uom = 'USD'"
        ),
        "restore_args": (original,),
    }


def _sec_q24_probe(row):
    if len(row) >= 9:
        # DuckDB-style: rowid + full num layout
        adsh, tag, version, original = row[1], row[2], row[3], row[8]
    else:
        # Postgres: ctid, adsh, tag, version, uom, value, ddate
        adsh, tag, version, original = row[1], row[2], row[3], row[5]
    delta = _sec_num_delta(original)
    return {
        "update": (
            f"UPDATE num SET value = value + {delta} "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND version = '{version}'"
        ),
        "restore": (
            "UPDATE num SET value = %s "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND version = '{version}'"
        ),
        "restore_args": (original,),
    }


def _tpch_seed_sql(query_id: int) -> str:
    root = Path(os.environ.get("TPCH_UTILS_ROOT", "/home/akhilvb/tpch-utils"))
    sql_path = root / "generated/sf1_instances/seed_1" / f"{query_id:02d}.sql"
    if not sql_path.is_file():
        raise FileNotFoundError(str(sql_path))
    sql = sql_path.read_text().strip()
    if query_id == 11:
        sql = sql.replace("* 1", "* 0.0001")
    if query_id == 22:
        sql = sql.replace(
            "IN (30, 19, 14, 28, 11, 22, 32)",
            "IN ('30', '19', '14', '28', '11', '22', '32')",
        )
    return sql


_TPCH_BASE_KERNEL_SQL: Dict[int, str] = {
    1: "SELECT * FROM accel_tpch_q1(90)",
    2: "SELECT * FROM accel_tpch_q2(10, 'BRASS', 'AFRICA')",
    3: "SELECT * FROM accel_tpch_q3('BUILDING', DATE '1995-03-15')",
    4: "SELECT * FROM accel_tpch_q4(DATE '1996-11-01')",
    5: "SELECT * FROM accel_tpch_q5('ASIA', DATE '1996-01-01')",
    6: "SELECT * FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)",
    7: "SELECT * FROM accel_tpch_q7('UNITED KINGDOM', 'BRAZIL')",
    8: "SELECT * FROM accel_tpch_q8('INDIA', 'ASIA', 'BRASS')",
    9: "SELECT * FROM accel_tpch_q9('powder')",
    10: "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')",
    11: "SELECT * FROM accel_tpch_q11('ROMANIA', 0.0001)",
    12: "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')",
    13: "SELECT * FROM accel_tpch_q13('%unusual%accounts%')",
    14: "SELECT * FROM accel_tpch_q14(DATE '1995-06-01')",
    15: "SELECT * FROM accel_tpch_q15(DATE '1994-11-01')",
    16: "SELECT * FROM accel_tpch_q16('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
    17: "SELECT * FROM accel_tpch_q17('Brand#34', 'LG CASE')",
    18: "SELECT * FROM accel_tpch_q18(312)",
    19: "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
    20: "SELECT * FROM accel_tpch_q20(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
    21: "SELECT * FROM accel_tpch_q21('ALGERIA')",
    22: "SELECT * FROM accel_tpch_q22('30', '19', '14', '28', '11', '22', '32')",
}


def _build_tpch_base_query_entry(query_id: int) -> SharedQueryEntry:
    existing = TPCH_QUERIES.get(query_id)
    if existing is not None:
        return SharedQueryEntry(
            existing.query_id,
            existing.workload,
            existing.base_sql,
            existing.aux_sql,
            _TPCH_BASE_KERNEL_SQL[query_id],
            existing.probe_select_sql,
            existing.mutation_table,
            existing.compare_unordered,
            existing.build_probe,
        )
    return SharedQueryEntry(
        query_id,
        "tpch",
        _tpch_seed_sql(query_id),
        f"SELECT * FROM accel_tpch_q{query_id}_aux()",
        _TPCH_BASE_KERNEL_SQL[query_id],
        "SELECT 1",
        1,
    )


TPCH_QUERIES: Dict[int, SharedQueryEntry] = {
    1: SharedQueryEntry(
        1,
        "tpch",
        """
SELECT l_returnflag, l_linestatus,
       sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price,
       sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price,
       avg(l_discount) AS avg_disc, count(*) AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus
""",
        "SELECT * FROM accel_tpch_q1_aux(90)",
        "SELECT * FROM accel_tpch_q1(90)",
        """
SELECT l_orderkey, l_linenumber, l_shipdate, l_quantity, l_extendedprice,
       l_discount, l_tax, l_returnflag, l_linestatus
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        1,
        build_probe=_tpch_q1_probe,
    ),
    3: SharedQueryEntry(
        3,
        "tpch",
        """
SELECT l_orderkey, revenue, o_orderdate, o_shippriority FROM (
SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue,
       o_orderdate, o_shippriority
FROM customer, orders, lineitem
WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey
  AND o_orderdate < DATE '1995-03-15' AND l_shipdate > DATE '1995-03-15'
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate, l_orderkey, o_shippriority LIMIT 10)
""",
        "SELECT * FROM accel_tpch_q3_aux('BUILDING', DATE '1995-03-15')",
        "SELECT * FROM accel_tpch_q3('BUILDING', DATE '1995-03-15')",
        """
SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate
FROM lineitem
WHERE l_orderkey IN (
  SELECT o_orderkey FROM customer JOIN orders ON c_custkey = o_custkey
  WHERE c_mktsegment = 'BUILDING' AND o_orderdate < DATE '1995-03-15')
  AND l_shipdate > DATE '1995-03-15'
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        3,
        build_probe=_lineitem_price_probe,
    ),
    6: SharedQueryEntry(
        6,
        "tpch",
        """
SELECT SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'
  AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25
""",
        "SELECT revenue FROM accel_tpch_q6_aux(DATE '1995-01-01', 0.03, 25)",
        "SELECT revenue FROM accel_tpch_q6(DATE '1995-01-01', 0.03, 25)",
        """
SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_quantity, l_shipdate
FROM lineitem
WHERE l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1996-01-01'
  AND l_discount BETWEEN 0.02 AND 0.04 AND l_quantity < 25
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        1,
        build_probe=_lineitem_price_probe,
    ),
    10: SharedQueryEntry(
        10,
        "tpch",
        """
SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue,
       c_acctbal, n_name, c_address, c_phone, c_comment
FROM customer JOIN orders ON c_custkey = o_custkey
JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON c_nationkey = n_nationkey
WHERE o_orderdate >= DATE '1993-06-01'
  AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH
  AND l_returnflag = 'R'
GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
ORDER BY revenue DESC, c_custkey LIMIT 20
""",
        "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
        "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_discount, l.l_returnflag
FROM lineitem l
JOIN orders o ON l.l_orderkey = o.o_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
WHERE l.l_returnflag = 'R'
  AND o.o_orderdate >= DATE '1993-06-01'
  AND o.o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH
  AND c.c_custkey IN (
    SELECT c_custkey FROM (
      SELECT c_custkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue
      FROM customer JOIN orders ON c_custkey = o_custkey
      JOIN lineitem ON l_orderkey = o_orderkey
      WHERE o_orderdate >= DATE '1993-06-01'
        AND o_orderdate < DATE '1993-06-01' + INTERVAL '3' MONTH
        AND l_returnflag = 'R'
      GROUP BY c_custkey ORDER BY revenue DESC, c_custkey LIMIT 20))
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        4,
        build_probe=_lineitem_price_probe,
    ),
    12: SharedQueryEntry(
        12,
        "tpch",
        """
SELECT l_shipmode,
       SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH'
                THEN 1 ELSE 0 END) AS high_line_count,
       SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH'
                THEN 1 ELSE 0 END) AS low_line_count
FROM orders, lineitem
WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP')
  AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate
  AND l_receiptdate >= DATE '1995-01-01'
  AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year
GROUP BY l_shipmode ORDER BY l_shipmode
""",
        "SELECT * FROM accel_tpch_q12_aux('MAIL', 'SHIP', DATE '1995-01-01')",
        "SELECT * FROM accel_tpch_q12('MAIL', 'SHIP', DATE '1995-01-01')",
        """
SELECT l_orderkey, l_linenumber, l_extendedprice, l_commitdate, l_receiptdate,
       l_shipdate, l_shipmode
FROM lineitem
WHERE l_shipmode IN ('MAIL', 'SHIP')
  AND l_receiptdate >= DATE '1995-01-01'
  AND l_receiptdate < DATE '1995-01-01' + INTERVAL '1' year
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        2,
        build_probe=_lineitem_receipt_probe,
    ),
    14: SharedQueryEntry(
        14,
        "tpch",
        """
SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%'
       THEN l_extendedprice * (1 - l_discount) ELSE 0 END) /
       SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= DATE '1995-06-01'
  AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month
""",
        "SELECT * FROM accel_tpch_q14_aux(DATE '1995-06-01')",
        "SELECT * FROM accel_tpch_q14(DATE '1995-06-01')",
        """
SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate, l_partkey
FROM lineitem
WHERE l_shipdate >= DATE '1995-06-01'
  AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        2,
        build_probe=_lineitem_price_probe,
    ),
    9: SharedQueryEntry(
        9,
        "tpch",
        """
SELECT nation, o_year, SUM(amount) AS sum_profit FROM (
SELECT n_name AS nation, EXTRACT(year FROM o_orderdate)::bigint AS o_year,
       l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
FROM part, supplier, lineitem, partsupp, orders, nation
WHERE s_suppkey = l_suppkey AND ps_suppkey = l_suppkey AND ps_partkey = l_partkey
  AND p_partkey = l_partkey AND o_orderkey = l_orderkey AND s_nationkey = n_nationkey
  AND p_name LIKE '%green%'
) AS profit
GROUP BY nation, o_year
ORDER BY nation, o_year DESC
""",
        "SELECT * FROM accel_tpch_q9_aux('green')",
        "SELECT * FROM accel_tpch_q9('green')",
        """
SELECT l_partkey, l_suppkey, l_orderkey, l_linenumber, l_quantity, l_extendedprice,
       l_discount
FROM lineitem
WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE '%green%')
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        6,
        build_probe=lambda row: _lineitem_quantity_probe(
            (row[2], row[3], row[4])
        ),
    ),
    11: SharedQueryEntry(
        11,
        "tpch",
        """
SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
FROM partsupp, supplier, nation
WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'ROMANIA'
GROUP BY ps_partkey
HAVING SUM(ps_supplycost * ps_availqty) > (
  SELECT SUM(ps_supplycost * ps_availqty) * 0.0001
  FROM partsupp, supplier, nation
  WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = 'ROMANIA')
ORDER BY value DESC
""",
        "SELECT * FROM accel_tpch_q11_aux('ROMANIA', 0.0001)",
        "SELECT * FROM accel_tpch_q11('ROMANIA', 0.0001)",
        """
SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_availqty, ps.ps_supplycost
FROM partsupp ps, supplier s, nation n
WHERE ps.ps_suppkey = s.s_suppkey AND s.s_nationkey = n.n_nationkey
  AND n.n_name = 'ROMANIA'
ORDER BY ps.ps_partkey, ps.ps_suppkey LIMIT 1
""",
        1,
        build_probe=_partsupp_cost_probe,
    ),
    17: SharedQueryEntry(
        17,
        "tpch",
        """
SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly
FROM lineitem, part,
(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity
 FROM lineitem GROUP BY l_partkey) part_agg
WHERE p_partkey = l_partkey AND agg_partkey = l_partkey
  AND p_brand = 'Brand#34' AND p_container = 'LG CASE'
  AND l_quantity < avg_quantity
""",
        "SELECT * FROM accel_tpch_q17_aux('Brand#34', 'LG CASE')",
        "SELECT * FROM accel_tpch_q17('Brand#34', 'LG CASE')",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice
FROM lineitem l, part p,
(SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity
 FROM lineitem GROUP BY l_partkey) part_agg
WHERE p.p_partkey = l.l_partkey AND part_agg.agg_partkey = l.l_partkey
  AND p.p_brand = 'Brand#34' AND p.p_container = 'LG CASE'
  AND l.l_quantity < part_agg.avg_quantity
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        2,
        build_probe=_lineitem_price_probe,
    ),
    18: SharedQueryEntry(
        18,
        "tpch",
        """
SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice,
       SUM(l_quantity) AS sum_qty
FROM customer, orders, lineitem
WHERE o_orderkey IN (
  SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > 312)
  AND c_custkey = o_custkey AND o_orderkey = l_orderkey
GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
ORDER BY o_totalprice DESC, o_orderdate ASC
""",
        "SELECT * FROM accel_tpch_q18_aux(312)",
        "SELECT * FROM accel_tpch_q18(312)",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_quantity
FROM lineitem l
WHERE l.l_orderkey IN (
  SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > 312)
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        0,
        build_probe=_lineitem_quantity_probe,
    ),
    21: SharedQueryEntry(
        21,
        "tpch",
        # Official Q21 is correlated EXISTS/NOT EXISTS over lineitem; Postgres
        # nested-loops that for minutes on SF1. This CTE is equivalent:
        # multi-supplier F-orders whose late receipts come from exactly one
        # supplier, counted per SAUDI ARABIA supplier line.
        """
WITH order_supp AS (
  SELECT l_orderkey,
         COUNT(DISTINCT l_suppkey) AS nsupp,
         COUNT(DISTINCT l_suppkey) FILTER (WHERE l_receiptdate > l_commitdate) AS nlate
  FROM lineitem
  GROUP BY l_orderkey
)
SELECT s.s_name, COUNT(*) AS numwait
FROM lineitem l1
JOIN orders o ON o.o_orderkey = l1.l_orderkey AND o.o_orderstatus = 'F'
JOIN order_supp os ON os.l_orderkey = l1.l_orderkey AND os.nsupp > 1 AND os.nlate = 1
JOIN supplier s ON s.s_suppkey = l1.l_suppkey
JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'SAUDI ARABIA'
WHERE l1.l_receiptdate > l1.l_commitdate
GROUP BY s.s_name
ORDER BY numwait DESC, s.s_name
""",
        "SELECT * FROM accel_tpch_q21_aux('SAUDI ARABIA')",
        "SELECT * FROM accel_tpch_q21('SAUDI ARABIA')",
        """
WITH order_supp AS (
  SELECT l_orderkey,
         COUNT(DISTINCT l_suppkey) AS nsupp,
         COUNT(DISTINCT l_suppkey) FILTER (WHERE l_receiptdate > l_commitdate) AS nlate
  FROM lineitem
  GROUP BY l_orderkey
)
SELECT l1.l_orderkey, l1.l_linenumber, l1.l_suppkey, l1.l_commitdate, l1.l_receiptdate
FROM lineitem l1
JOIN orders o ON o.o_orderkey = l1.l_orderkey AND o.o_orderstatus = 'F'
JOIN order_supp os ON os.l_orderkey = l1.l_orderkey AND os.nsupp > 1 AND os.nlate = 1
JOIN supplier s ON s.s_suppkey = l1.l_suppkey
JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'SAUDI ARABIA'
WHERE l1.l_receiptdate > l1.l_commitdate
ORDER BY l1.l_orderkey, l1.l_linenumber LIMIT 1
""",
        3,
        build_probe=_lineitem_unlate_probe,
    ),
    22: SharedQueryEntry(
        22,
        "tpch",
        """
WITH custsale AS (
  SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
  FROM customer
  WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32')
    AND c_acctbal > (
      SELECT AVG(c_acctbal) FROM customer
      WHERE c_acctbal > 0.00
        AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32'))
    AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)
)
SELECT cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
FROM custsale GROUP BY cntrycode ORDER BY cntrycode
""",
        "SELECT * FROM accel_tpch_q22_aux('30', '19', '14', '28', '11', '22', '32')",
        "SELECT * FROM accel_tpch_q22('30', '19', '14', '28', '11', '22', '32')",
        """
SELECT c_custkey, c_acctbal
FROM customer
WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32')
  AND c_acctbal > (
    SELECT AVG(c_acctbal) FROM customer
    WHERE c_acctbal > 0.00
      AND SUBSTRING(c_phone FROM 1 FOR 2) IN ('30', '19', '14', '28', '11', '22', '32'))
  AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = customer.c_custkey)
ORDER BY c_custkey LIMIT 1
""",
        9,
        build_probe=_customer_acctbal_probe,
    ),
    2: SharedQueryEntry(
        2,
        "tpch",
        """
SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
FROM part, supplier, partsupp, nation, region
WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = 15
  AND p_type LIKE '%BRASS' AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
  AND r_name = 'EUROPE'
  AND ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier, nation, region
    WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey
      AND n_regionkey = r_regionkey AND r_name = 'EUROPE')
ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100
""",
        "SELECT * FROM accel_tpch_q2_aux(15, '%BRASS', 'EUROPE')",
        "SELECT * FROM accel_tpch_q2(15, '%BRASS', 'EUROPE')",
        """
SELECT ps.ps_partkey, ps.ps_suppkey, ps.ps_supplycost
FROM part p, supplier s, partsupp ps, nation n, region r
WHERE p.p_partkey = ps.ps_partkey AND s.s_suppkey = ps.ps_suppkey
  AND p.p_size = 15 AND p.p_type LIKE '%BRASS'
  AND s.s_nationkey = n.n_nationkey AND n.n_regionkey = r.r_regionkey
  AND r.r_name = 'EUROPE'
  AND ps.ps_supplycost = (SELECT MIN(ps_supplycost) FROM partsupp, supplier,
    nation, region WHERE p.p_partkey = ps_partkey AND s_suppkey = ps_suppkey
    AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
    AND r_name = 'EUROPE')
  AND EXISTS (
    SELECT 1 FROM partsupp ps2, supplier s2, nation n2, region r2
    WHERE ps2.ps_partkey = p.p_partkey AND ps2.ps_suppkey <> ps.ps_suppkey
      AND s2.s_suppkey = ps2.ps_suppkey AND s2.s_nationkey = n2.n_nationkey
      AND n2.n_regionkey = r2.r_regionkey AND r2.r_name = 'EUROPE')
ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey LIMIT 1
""",
        3,
        build_probe=_partsupp_cost_large_probe,
    ),
    4: SharedQueryEntry(
        4,
        "tpch",
        """
SELECT o_orderpriority, COUNT(o_orderkey) AS order_count
FROM orders
WHERE o_orderdate >= DATE '1993-07-01'
  AND o_orderdate < DATE '1993-07-01' + INTERVAL '3' month
  AND EXISTS (
    SELECT * FROM lineitem WHERE l_orderkey = o_orderkey
      AND l_commitdate < l_receiptdate)
GROUP BY o_orderpriority ORDER BY o_orderpriority
""",
        "SELECT * FROM accel_tpch_q4_aux(DATE '1993-07-01')",
        "SELECT * FROM accel_tpch_q4(DATE '1993-07-01')",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_commitdate, l.l_receiptdate
FROM lineitem l
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN (
  SELECT l_orderkey FROM lineitem
  WHERE l_commitdate < l_receiptdate
  GROUP BY l_orderkey HAVING COUNT(*) = 1
) solo ON solo.l_orderkey = l.l_orderkey
WHERE o.o_orderdate >= DATE '1993-07-01'
  AND o.o_orderdate < DATE '1993-07-01' + INTERVAL '3' month
  AND l.l_commitdate < l.l_receiptdate
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        2,
        build_probe=_q4_unlate_probe,
    ),
    5: SharedQueryEntry(
        5,
        "tpch",
        """
SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM customer, orders, lineitem, supplier, nation, region
WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey
  AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey
  AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey
  AND r_name = 'AMERICA'
  AND o_orderdate >= DATE '1995-01-01'
  AND o_orderdate < DATE '1995-01-01' + INTERVAL '1' year
GROUP BY n_name ORDER BY revenue DESC
""",
        "SELECT * FROM accel_tpch_q5_aux('AMERICA', DATE '1995-01-01')",
        "SELECT * FROM accel_tpch_q5('AMERICA', DATE '1995-01-01')",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_discount
FROM lineitem l
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
JOIN supplier s ON s.s_suppkey = l.l_suppkey AND s.s_nationkey = c.c_nationkey
JOIN nation n ON n.n_nationkey = s.s_nationkey
JOIN region r ON r.r_regionkey = n.n_regionkey
WHERE r.r_name = 'AMERICA'
  AND o.o_orderdate >= DATE '1995-01-01'
  AND o.o_orderdate < DATE '1995-01-01' + INTERVAL '1' year
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        6,
        build_probe=_lineitem_price_probe,
    ),
    7: SharedQueryEntry(
        7,
        "tpch",
        """
SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue FROM (
SELECT n1.n_name AS supp_nation, n2.n_name AS cust_nation,
       EXTRACT(year FROM l_shipdate)::bigint AS l_year,
       l_extendedprice * (1 - l_discount) AS volume
FROM supplier, lineitem, orders, customer, nation n1, nation n2
WHERE s_suppkey = l_suppkey AND o_orderkey = l_orderkey
  AND c_custkey = o_custkey AND s_nationkey = n1.n_nationkey
  AND c_nationkey = n2.n_nationkey
  AND ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL')
    OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM'))
  AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31') AS shipping
GROUP BY supp_nation, cust_nation, l_year
ORDER BY supp_nation, cust_nation, l_year
""",
        "SELECT * FROM accel_tpch_q7_aux('UNITED KINGDOM', 'BRAZIL')",
        "SELECT * FROM accel_tpch_q7('UNITED KINGDOM', 'BRAZIL')",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_extendedprice, l.l_discount
FROM lineitem l
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
JOIN supplier s ON s.s_suppkey = l.l_suppkey
JOIN nation n1 ON n1.n_nationkey = s.s_nationkey
JOIN nation n2 ON n2.n_nationkey = c.c_nationkey
WHERE ((n1.n_name = 'UNITED KINGDOM' AND n2.n_name = 'BRAZIL')
    OR (n1.n_name = 'BRAZIL' AND n2.n_name = 'UNITED KINGDOM'))
  AND l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        4,
        build_probe=_lineitem_price_probe,
    ),
    8: SharedQueryEntry(
        8,
        "tpch",
        """
SELECT o_year, SUM(CASE WHEN nation = 'INDIA' THEN volume ELSE 0 END) /
       SUM(volume) AS mkt_share FROM (
SELECT EXTRACT(year FROM o_orderdate)::bigint AS o_year,
       l_extendedprice * (1 - l_discount) AS volume, n2.n_name AS nation
FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey
  AND l_orderkey = o_orderkey AND o_custkey = c_custkey
  AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey
  AND r_name = 'ASIA' AND s_nationkey = n2.n_nationkey
  AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
  AND p_type = 'PROMO BRUSHED BRASS') AS all_nations
GROUP BY o_year ORDER BY o_year
""",
        "SELECT * FROM accel_tpch_q8_aux('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
        "SELECT * FROM accel_tpch_q8('INDIA', 'ASIA', 'PROMO BRUSHED BRASS')",
        """
SELECT l.l_partkey, l.l_orderkey, l.l_suppkey, l.l_extendedprice,
       l.l_discount, l.l_linenumber
FROM lineitem l
JOIN part p ON p.p_partkey = l.l_partkey AND p.p_type = 'PROMO BRUSHED BRASS'
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
JOIN nation n1 ON n1.n_nationkey = c.c_nationkey
JOIN region r ON r.r_regionkey = n1.n_regionkey AND r.r_name = 'ASIA'
WHERE o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        4,
        build_probe=_lineitem_price_large_probe,
    ),
    13: SharedQueryEntry(
        13,
        "tpch",
        """
SELECT c_count, COUNT(*) AS custdist FROM (
SELECT c_custkey, COUNT(o_orderkey) AS c_count FROM customer
LEFT OUTER JOIN orders ON c_custkey = o_custkey AND o_comment NOT LIKE
  '%unusual%accounts%' GROUP BY c_custkey) AS c_orders
GROUP BY c_count ORDER BY custdist DESC, c_count DESC
""",
        "SELECT * FROM accel_tpch_q13_aux('%unusual%accounts%')",
        "SELECT * FROM accel_tpch_q13('%unusual%accounts%')",
        """
SELECT o_orderkey, o_custkey, o_comment FROM orders
WHERE o_comment NOT LIKE '%unusual%accounts%'
ORDER BY o_orderkey LIMIT 1
""",
        1,
        build_probe=_orders_comment_probe,
    ),
    15: SharedQueryEntry(
        15,
        "tpch",
        """
SELECT s_suppkey, s_name, s_address, s_phone, total_revenue
FROM supplier, (
  SELECT l_suppkey AS supplier_no,
         SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
  FROM lineitem WHERE l_shipdate >= DATE '1994-11-01'
    AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH
  GROUP BY l_suppkey) revenue0
WHERE s_suppkey = supplier_no
  AND total_revenue = (SELECT MAX(total_revenue) FROM (
    SELECT l_suppkey AS supplier_no,
           SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
    FROM lineitem WHERE l_shipdate >= DATE '1994-11-01'
      AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH
    GROUP BY l_suppkey) revenue1)
ORDER BY s_suppkey
""",
        "SELECT * FROM accel_tpch_q15_aux(DATE '1994-11-01')",
        "SELECT * FROM accel_tpch_q15(DATE '1994-11-01')",
        """
SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_shipdate, l_suppkey
FROM lineitem
WHERE l_shipdate >= DATE '1994-11-01'
  AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH
  AND l_suppkey = (SELECT l_suppkey FROM lineitem
    WHERE l_shipdate >= DATE '1994-11-01'
      AND l_shipdate < DATE '1994-11-01' + INTERVAL '3' MONTH
    GROUP BY l_suppkey
    ORDER BY SUM(l_extendedprice * (1 - l_discount)) DESC LIMIT 1)
ORDER BY l_orderkey, l_linenumber LIMIT 1
""",
        1,
        build_probe=_lineitem_price_probe,
    ),
    16: SharedQueryEntry(
        16,
        "tpch",
        """
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey AND p_brand <> 'Brand#13'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33)
  AND ps_suppkey NOT IN (
    SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
""",
        "SELECT * FROM accel_tpch_q16_aux('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
        "SELECT * FROM accel_tpch_q16('Brand#13', 'SMALL BRUSHED%', 26, 24, 27, 29, 7, 20, 22, 33)",
        """
SELECT p_partkey, p_brand FROM part
WHERE p_size IN (26, 24, 27, 29, 7, 20, 22, 33)
  AND p_brand <> 'Brand#13' AND p_brand <> 'Brand#00'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
ORDER BY p_partkey LIMIT 1
""",
        3,
        build_probe=_part_brand_new_group_probe,
    ),
    19: SharedQueryEntry(
        19,
        "tpch",
        """
SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
FROM lineitem, part
WHERE (p_partkey = l_partkey
  AND p_brand = 'Brand#31'
  AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
  AND l_quantity >= 4 AND l_quantity <= 4 + 10
  AND p_size BETWEEN 1 AND 5
  AND l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON')
OR (p_partkey = l_partkey
  AND p_brand = 'Brand#12'
  AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
  AND l_quantity >= 15 AND l_quantity <= 15 + 10
  AND p_size BETWEEN 1 AND 10
  AND l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON')
OR (p_partkey = l_partkey
  AND p_brand = 'Brand#51'
  AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
  AND l_quantity >= 23 AND l_quantity <= 23 + 10
  AND p_size BETWEEN 1 AND 15
  AND l_shipmode IN ('AIR', 'AIR REG')
  AND l_shipinstruct = 'DELIVER IN PERSON')
""",
        "SELECT * FROM accel_tpch_q19_aux('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
        "SELECT * FROM accel_tpch_q19('Brand#31', 4, 'Brand#12', 15, 'Brand#51', 23)",
        """
SELECT l.l_orderkey, l.l_partkey, l.l_linenumber, l.l_quantity,
       l.l_extendedprice, l.l_discount, l.l_shipmode, l.l_shipinstruct
FROM lineitem l, part p
WHERE p.p_partkey = l.l_partkey AND p.p_brand = 'Brand#31'
  AND p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
  AND l.l_quantity >= 4 AND l.l_quantity <= 14
  AND p.p_size BETWEEN 1 AND 5
  AND l.l_shipmode IN ('AIR', 'AIR REG')
  AND l.l_shipinstruct = 'DELIVER IN PERSON'
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        5,
        build_probe=_lineitem_quantity_large_probe,
    ),
    20: SharedQueryEntry(
        20,
        "tpch",
        """
SELECT s_name, s_address
FROM supplier, nation
WHERE s_suppkey IN (
  SELECT ps_suppkey FROM partsupp, (
    SELECT l_partkey agg_partkey, l_suppkey agg_suppkey, 0.5 * SUM(l_quantity) AS agg_quantity
    FROM lineitem WHERE l_shipdate >= DATE '1997-01-01'
      AND l_shipdate < DATE '1997-01-01' + INTERVAL '1' year
    GROUP BY l_partkey, l_suppkey) agg_lineitem
  WHERE agg_partkey = ps_partkey AND agg_suppkey = ps_suppkey
    AND ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'gainsboro%')
    AND ps_availqty > agg_quantity)
  AND s_nationkey = n_nationkey AND n_name = 'RUSSIA'
ORDER BY s_name
""",
        "SELECT * FROM accel_tpch_q20_aux(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
        "SELECT * FROM accel_tpch_q20(DATE '1997-01-01', 'gainsboro%', 'RUSSIA')",
        """
SELECT l.l_orderkey, l.l_linenumber, l.l_quantity, l.l_shipdate,
       l.l_partkey, l.l_suppkey FROM lineitem l
JOIN part p ON p.p_partkey = l.l_partkey AND p.p_name LIKE 'gainsboro%'
JOIN partsupp ps ON ps.ps_partkey = l.l_partkey AND ps.ps_suppkey = l.l_suppkey
JOIN supplier s ON s.s_suppkey = l.l_suppkey
JOIN nation n ON n.n_nationkey = s.s_nationkey AND n.n_name = 'RUSSIA'
WHERE l.l_shipdate >= DATE '1997-01-01'
  AND l.l_shipdate < DATE '1998-01-01'
ORDER BY l.l_orderkey, l.l_linenumber LIMIT 1
""",
        1,
        build_probe=_lineitem_quantity_huge_probe,
    ),
}

SEC_EDGAR_QUERIES: Dict[int, SharedQueryEntry] = {
    1: SharedQueryEntry(
        1,
        "sec_edgar",
        """
SELECT stmt, rfile, COUNT(*) AS cnt, COUNT(DISTINCT adsh) AS num_filings,
       AVG(line) AS avg_line_num
FROM pre WHERE stmt IS NOT NULL
GROUP BY stmt, rfile ORDER BY cnt DESC
""",
        "SELECT * FROM accel_sec_edgar_q1_aux()",
        "SELECT * FROM accel_sec_edgar_q1()",
        "SELECT adsh, line, stmt, rfile FROM pre WHERE stmt IS NOT NULL "
        "ORDER BY adsh, line LIMIT 1",
        1,
        build_probe=_sec_q1_probe,
    ),
    2: SharedQueryEntry(
        2,
        "sec_edgar",
        """
SELECT s.name, n.tag, n.value
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN (
    SELECT adsh, tag, MAX(value) AS max_value
    FROM num
    WHERE uom = 'pure' AND value IS NOT NULL
    GROUP BY adsh, tag
) m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value
WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL
ORDER BY n.value DESC, s.name, n.tag
LIMIT 100
""",
        "SELECT * FROM accel_sec_edgar_q2_aux()",
        "SELECT * FROM accel_sec_edgar_q2()",
        """
SELECT n.adsh, n.tag, n.value, n.uom FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN (
    SELECT adsh, tag, MAX(value) AS max_value
    FROM num
    WHERE uom = 'pure' AND value IS NOT NULL
    GROUP BY adsh, tag
) m ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value
WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL
ORDER BY n.value DESC, s.name, n.tag LIMIT 1
""",
        1,
        build_probe=_sec_q2_probe,
    ),
    3: SharedQueryEntry(
        3,
        "sec_edgar",
        """
SELECT s.name, s.cik, SUM(n.value) AS total_value
FROM num n
JOIN sub s ON n.adsh = s.adsh
WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL
GROUP BY s.name, s.cik
HAVING SUM(n.value) > (
    SELECT AVG(sub_total) FROM (
        SELECT SUM(n2.value) AS sub_total
        FROM num n2
        JOIN sub s2 ON n2.adsh = s2.adsh
        WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL
        GROUP BY s2.cik
    ) avg_sub
)
ORDER BY total_value DESC
LIMIT 100
""",
        "SELECT * FROM accel_sec_edgar_q3_aux()",
        "SELECT * FROM accel_sec_edgar_q3()",
        """
SELECT n.adsh, n.value, n.uom FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN (
    SELECT s2.cik FROM num n2
    JOIN sub s2 ON n2.adsh = s2.adsh
    WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL
    GROUP BY s2.name, s2.cik
    HAVING SUM(n2.value) > (
        SELECT AVG(sub_total) FROM (
            SELECT SUM(n3.value) AS sub_total
            FROM num n3 JOIN sub s3 ON n3.adsh = s3.adsh
            WHERE n3.uom = 'USD' AND s3.fy = 2022 AND n3.value IS NOT NULL
            GROUP BY s3.cik
        ) avg_sub
    )
    ORDER BY SUM(n2.value) DESC
    LIMIT 100
) top ON s.cik = top.cik
WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL
ORDER BY n.adsh LIMIT 1
""",
        1,
        build_probe=_sec_q3_probe,
    ),
    4: SharedQueryEntry(
        4,
        "sec_edgar",
        """
SELECT s.sic, t.tlabel, p.stmt,
       COUNT(DISTINCT s.cik) AS num_companies,
       SUM(n.value) AS total_value,
       AVG(n.value) AS avg_value
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN tag t ON n.tag = t.tag AND n.version = t.version
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
WHERE n.uom = 'USD' AND p.stmt = 'EQ'
  AND s.sic BETWEEN 4000 AND 4999
  AND n.value IS NOT NULL AND t.abstract = 0
GROUP BY s.sic, t.tlabel, p.stmt
HAVING COUNT(DISTINCT s.cik) >= 2
ORDER BY total_value DESC
LIMIT 500
""",
        "SELECT * FROM accel_sec_edgar_q4_aux()",
        "SELECT * FROM accel_sec_edgar_q4()",
        """
SELECT n.adsh, n.tag, n.version, n.value, n.uom FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
JOIN tag t ON n.tag = t.tag AND n.version = t.version
JOIN (
    SELECT s2.sic, t2.tlabel, p2.stmt FROM num n2
    JOIN sub s2 ON n2.adsh = s2.adsh
    JOIN tag t2 ON n2.tag = t2.tag AND n2.version = t2.version
    JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version
    WHERE n2.uom = 'USD' AND p2.stmt = 'EQ' AND s2.sic BETWEEN 4000 AND 4999
      AND n2.value IS NOT NULL AND t2.abstract = 0
    GROUP BY s2.sic, t2.tlabel, p2.stmt
    HAVING COUNT(DISTINCT s2.cik) >= 2
    ORDER BY SUM(n2.value) DESC
    LIMIT 500
) top ON s.sic = top.sic AND t.tlabel = top.tlabel AND p.stmt = top.stmt
WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999
  AND n.value IS NOT NULL AND t.abstract = 0
ORDER BY n.adsh, n.tag, n.version LIMIT 1
""",
        1,
        build_probe=_sec_num_probe,
    ),
    6: SharedQueryEntry(
        6,
        "sec_edgar",
        """
SELECT s.name, p.stmt, n.tag, p.plabel,
       SUM(n.value) AS total_value, COUNT(*) AS cnt
FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023
  AND n.value IS NOT NULL
GROUP BY s.name, p.stmt, n.tag, p.plabel
ORDER BY total_value DESC, s.name, n.tag, p.plabel
LIMIT 200
""",
        "SELECT * FROM accel_sec_edgar_q6_aux()",
        "SELECT * FROM accel_sec_edgar_q6()",
        """
SELECT n.adsh, n.tag, n.version, n.value, n.uom FROM num n
JOIN sub s ON n.adsh = s.adsh
JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version
JOIN (
    SELECT s2.name, p2.stmt, n2.tag, p2.plabel FROM num n2
    JOIN sub s2 ON n2.adsh = s2.adsh
    JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version
    WHERE n2.uom = 'USD' AND p2.stmt = 'IS' AND s2.fy = 2023
      AND n2.value IS NOT NULL
    GROUP BY s2.name, p2.stmt, n2.tag, p2.plabel
    ORDER BY SUM(n2.value) DESC, s2.name, n2.tag, p2.plabel
    LIMIT 200
) top ON s.name = top.name AND p.stmt = top.stmt AND n.tag = top.tag
   AND p.plabel IS NOT DISTINCT FROM top.plabel
WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023
  AND n.value IS NOT NULL
ORDER BY n.adsh, n.tag, n.version LIMIT 1
""",
        1,
        build_probe=_sec_num_probe,
    ),
    24: SharedQueryEntry(
        24,
        "sec_edgar",
        """
SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total
FROM num n LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version
  AND n.adsh = p.adsh
WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231
  AND n.value IS NOT NULL AND p.adsh IS NULL
GROUP BY n.tag, n.version HAVING COUNT(*) > 10
ORDER BY cnt DESC LIMIT 100
""",
        "SELECT * FROM accel_sec_edgar_q24_aux()",
        "SELECT * FROM accel_sec_edgar_q24()",
        """
SELECT n.ctid, n.adsh, n.tag, n.version, n.uom, n.value, n.ddate
FROM num n LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version
  AND n.adsh = p.adsh
WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231
  AND n.value IS NOT NULL AND p.adsh IS NULL
  AND (n.tag, n.version) IN (
    SELECT n2.tag, n2.version FROM num n2
    LEFT JOIN pre p2 ON n2.tag = p2.tag AND n2.version = p2.version
      AND n2.adsh = p2.adsh
    WHERE n2.uom = 'USD' AND n2.ddate BETWEEN 20230101 AND 20231231
      AND n2.value IS NOT NULL AND p2.adsh IS NULL
    GROUP BY n2.tag, n2.version HAVING COUNT(*) > 10
    ORDER BY COUNT(*) DESC
    LIMIT 100
  )
ORDER BY n.value DESC LIMIT 1
""",
        101,
        True,
        build_probe=_sec_q24_probe,
    ),
}


def _safe_tpch_base_entry(query_id: int) -> Optional[SharedQueryEntry]:
    try:
        return _build_tpch_base_query_entry(query_id)
    except FileNotFoundError:
        return None


TPCH_BASE_KERNEL_QUERIES: Dict[int, SharedQueryEntry] = {
    query_id: entry
    for query_id in _TPCH_BASE_KERNEL_SQL
    if (entry := _safe_tpch_base_entry(query_id)) is not None
}


def lookup_aux_query(workload: str, query_id: int) -> SharedQueryEntry:
    table = TPCH_QUERIES if workload == "tpch" else SEC_EDGAR_QUERIES
    if query_id not in table:
        raise KeyError(f"unsupported shared query {workload} Q{query_id}")
    return table[query_id]


def lookup_base_kernel_query(workload: str, query_id: int) -> SharedQueryEntry:
    if workload == "tpch":
        table = TPCH_BASE_KERNEL_QUERIES
    else:
        table = SEC_EDGAR_QUERIES
    if query_id not in table:
        raise KeyError(f"unsupported base kernel query {workload} Q{query_id}")
    return table[query_id]


def lookup_query(workload: str, query_id: int) -> SharedQueryEntry:
    return lookup_aux_query(workload, query_id)


def lookup_parallel_query(workload: str, query_id: int) -> SharedQueryEntry:
    if workload != "sec_edgar":
        raise KeyError("parallel aux catalog is only implemented for sec_edgar")
    entry = lookup_aux_query(workload, query_id)
    return SharedQueryEntry(
        query_id=entry.query_id,
        workload=entry.workload,
        base_sql=entry.base_sql,
        aux_sql=f"SELECT * FROM accel_sec_edgar_q{query_id}_aux_parallel()",
        kernel_sql=entry.kernel_sql,
        probe_select_sql=entry.probe_select_sql,
        mutation_table=entry.mutation_table,
        compare_unordered=entry.compare_unordered,
        build_probe=entry.build_probe,
    )


def queries_for_workload(workload: str) -> List[int]:
    if workload == "tpch":
        return sorted(TPCH_QUERIES.keys())
    if workload == "sec_edgar":
        return sorted(SEC_EDGAR_QUERIES.keys())
    raise KeyError(workload)


def tpch_rf_query_ids() -> List[int]:
    # Q11 is partsupp/supplier/nation only; RF1/RF2 never invalidate it.
    return [query_id for query_id in queries_for_workload("tpch") if query_id != 11]


def base_kernel_queries_for_workload(workload: str) -> List[int]:
    if workload == "tpch":
        return sorted(_TPCH_BASE_KERNEL_SQL.keys())
    if workload == "sec_edgar":
        return [1, 2, 3, 4, 6, 24]
    raise KeyError(workload)
