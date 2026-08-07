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


def _sec_num_probe(row):
    adsh, tag, version, original, *_ = row
    return {
        "update": (
            "UPDATE num SET value = value + 0.001 "
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
    adsh, tag, original, _uom = row
    return {
        "update": (
            "UPDATE num SET value = value + 0.001 "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND uom = 'pure'"
        ),
        "restore": (
            "UPDATE num SET value = %s "
            f"WHERE adsh = '{adsh}' AND tag = '{tag}' AND uom = 'pure'"
        ),
        "restore_args": (original,),
    }


def _sec_q3_probe(row):
    adsh, original, _uom = row
    return {
        "update": (
            "UPDATE num SET value = value + 0.001 "
            f"WHERE adsh = '{adsh}' AND uom = 'USD'"
        ),
        "restore": (
            "UPDATE num SET value = %s "
            f"WHERE adsh = '{adsh}' AND uom = 'USD'"
        ),
        "restore_args": (original,),
    }


def _sec_q24_probe(row):
    rowid, adsh, tag, version, _uom, original, _ddate = row
    return {
        "update": f"UPDATE num SET value = value + 0.001 WHERE rowid = {rowid}",
        "restore": f"UPDATE num SET value = %s WHERE rowid = {rowid}",
        "restore_args": (original,),
        "key_sql": None,
    }


def _tpch_seed_sql(query_id: int) -> str:
    root = Path(os.environ.get("TPCH_UTILS_ROOT", "/home/akhilvb/tpch-utils"))
    sql_path = root / "generated/sf1_instances/seed_1" / f"{query_id:02d}.sql"
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
    17: "SELECT * FROM accel_tpch_q17('Brand#34', 'LARGE BRUSHED')",
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
ORDER BY revenue DESC, c_custkey
""",
        "SELECT * FROM accel_tpch_q10_aux(DATE '1993-06-01')",
        "SELECT * FROM accel_tpch_q10(DATE '1993-06-01')",
        """
SELECT l_orderkey, l_linenumber, l_extendedprice, l_discount, l_returnflag
FROM lineitem WHERE l_returnflag = 'R'
ORDER BY l_orderkey, l_linenumber LIMIT 1
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
        build_probe=_lineitem_price_probe,
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
WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL
ORDER BY n.adsh, n.tag, n.value DESC LIMIT 1
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
ORDER BY n.adsh, n.tag, n.version LIMIT 1
""",
        101,
        True,
        build_probe=lambda row: {
            "update": (
                "UPDATE num SET value = value + 0.001 "
                f"WHERE adsh = '{row[1]}' AND tag = '{row[2]}' AND version = '{row[3]}'"
            ),
            "restore": (
                "UPDATE num SET value = %s "
                f"WHERE adsh = '{row[1]}' AND tag = '{row[2]}' AND version = '{row[3]}'"
            ),
            "restore_args": (row[5],),
        },
    ),
}


TPCH_BASE_KERNEL_QUERIES: Dict[int, SharedQueryEntry] = {
    query_id: _build_tpch_base_query_entry(query_id) for query_id in _TPCH_BASE_KERNEL_SQL
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


def queries_for_workload(workload: str) -> List[int]:
    if workload == "tpch":
        return [1, 3, 6, 10, 12, 14]
    if workload == "sec_edgar":
        return [1, 2, 3, 4, 6, 24]
    raise KeyError(workload)


def base_kernel_queries_for_workload(workload: str) -> List[int]:
    if workload == "tpch":
        return sorted(_TPCH_BASE_KERNEL_SQL.keys())
    if workload == "sec_edgar":
        return [1, 2, 3, 4, 6, 24]
    raise KeyError(workload)
