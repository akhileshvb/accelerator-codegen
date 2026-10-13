#!/usr/bin/env python3
"""Generate pg_ivm IMMV SQL for TPC-H queries (third baseline vs REFRESH matview / aux).

IMMV definitions mirror matview grains where pg_ivm allows. Unsupported queries
get unsupported.txt so apply/bench can skip cleanly.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src" / "generated" / "tpch"

# view_definition only (no CREATE) — fed to pgivm.create_immv
DEFINITIONS: dict[int, str] = {
    1: """
SELECT
  l_shipdate,
  l_returnflag,
  l_linestatus,
  SUM(l_quantity) AS sum_qty,
  SUM(l_extendedprice) AS sum_base_price,
  SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
  SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
  SUM(l_discount) AS sum_discount,
  COUNT(*) AS count_order
FROM lineitem
GROUP BY l_shipdate, l_returnflag, l_linestatus
""".strip(),
    3: """
SELECT
  c.c_mktsegment,
  o.o_orderdate,
  l.l_shipdate,
  l.l_orderkey,
  o.o_shippriority,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM customer c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
GROUP BY c.c_mktsegment, o.o_orderdate, l.l_shipdate, l.l_orderkey, o.o_shippriority
""".strip(),
    5: """
SELECT
  r.r_name,
  n.n_name,
  o.o_orderdate,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM customer c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
JOIN supplier s ON l.l_suppkey = s.s_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
JOIN region r ON n.n_regionkey = r.r_regionkey
WHERE c.c_nationkey = s.s_nationkey
GROUP BY r.r_name, n.n_name, o.o_orderdate
""".strip(),
    6: """
SELECT
  l_shipdate,
  l_discount,
  l_quantity,
  SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
GROUP BY l_shipdate, l_discount, l_quantity
""".strip(),
    7: """
SELECT
  n1.n_name AS supp_nation,
  n2.n_name AS cust_nation,
  EXTRACT(year FROM l.l_shipdate)::integer AS l_year,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM supplier s
JOIN lineitem l ON s.s_suppkey = l.l_suppkey
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
JOIN nation n1 ON s.s_nationkey = n1.n_nationkey
JOIN nation n2 ON c.c_nationkey = n2.n_nationkey
WHERE l.l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
GROUP BY n1.n_name, n2.n_name, EXTRACT(year FROM l.l_shipdate)::integer
""".strip(),
    8: """
SELECT
  r.r_name,
  p.p_type,
  EXTRACT(year FROM o.o_orderdate)::integer AS o_year,
  n2.n_name AS nation,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS volume
FROM part p
JOIN lineitem l ON p.p_partkey = l.l_partkey
JOIN supplier s ON s.s_suppkey = l.l_suppkey
JOIN orders o ON l.l_orderkey = o.o_orderkey
JOIN customer c ON o.o_custkey = c.c_custkey
JOIN nation n1 ON c.c_nationkey = n1.n_nationkey
JOIN nation n2 ON s.s_nationkey = n2.n_nationkey
JOIN region r ON n1.n_regionkey = r.r_regionkey
WHERE o.o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
GROUP BY r.r_name, p.p_type, EXTRACT(year FROM o.o_orderdate)::integer, n2.n_name
""".strip(),
    10: """
SELECT
  c.c_custkey,
  c.c_name,
  c.c_acctbal,
  c.c_phone,
  c.c_address,
  c.c_comment,
  n.n_name,
  o.o_orderdate,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM customer c
JOIN orders o ON c.c_custkey = o.o_custkey
JOIN lineitem l ON l.l_orderkey = o.o_orderkey
JOIN nation n ON c.c_nationkey = n.n_nationkey
WHERE l.l_returnflag = 'R'
GROUP BY c.c_custkey, c.c_name, c.c_acctbal, c.c_phone, c.c_address, c.c_comment,
         n.n_name, o.o_orderdate
""".strip(),
    11: """
SELECT
  n.n_name,
  ps.ps_partkey,
  SUM(ps.ps_supplycost * ps.ps_availqty) AS value
FROM partsupp ps
JOIN supplier s ON ps.ps_suppkey = s.s_suppkey
JOIN nation n ON s.s_nationkey = n.n_nationkey
GROUP BY n.n_name, ps.ps_partkey
""".strip(),
    12: """
SELECT
  l.l_shipmode,
  l.l_receiptdate,
  CASE
    WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1
    ELSE 0
  END AS is_high,
  COUNT(*) AS line_count
FROM orders o
JOIN lineitem l ON o.o_orderkey = l.l_orderkey
WHERE l.l_commitdate < l.l_receiptdate
  AND l.l_shipdate < l.l_commitdate
GROUP BY l.l_shipmode, l.l_receiptdate,
         CASE WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END
""".strip(),
    14: """
SELECT
  l.l_shipdate,
  SUM(CASE WHEN p.p_type LIKE 'PROMO%' THEN l.l_extendedprice * (1 - l.l_discount) ELSE 0 END) AS promo_rev,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS all_rev
FROM lineitem l
JOIN part p ON l.l_partkey = p.p_partkey
GROUP BY l.l_shipdate
""".strip(),
    15: """
SELECT
  l.l_suppkey AS supplier_no,
  l.l_shipdate,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS total_revenue
FROM lineitem l
GROUP BY l.l_suppkey, l.l_shipdate
""".strip(),
    19: """
SELECT
  p.p_brand,
  CASE
    WHEN p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') THEN 'SM'
    WHEN p.p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') THEN 'MED'
    WHEN p.p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') THEN 'LG'
    ELSE 'OTHER'
  END AS cont_class,
  l.l_quantity,
  p.p_size,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM lineitem l
JOIN part p ON p.p_partkey = l.l_partkey
WHERE l.l_shipmode IN ('AIR', 'AIR REG')
  AND l.l_shipinstruct = 'DELIVER IN PERSON'
GROUP BY
  p.p_brand,
  CASE
    WHEN p.p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') THEN 'SM'
    WHEN p.p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') THEN 'MED'
    WHEN p.p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') THEN 'LG'
    ELSE 'OTHER'
  END,
  l.l_quantity,
  p.p_size
""".strip(),
}

UNSUPPORTED: dict[int, str] = {
    2: "CTE with MIN aggregate join (pg_ivm forbids agg inside FROM/CTE subquery)",
    4: "aggregate + EXISTS not supported together by pg_ivm",
    9: "LATERAL unnest of p_name tokens unsupported",
    13: "array_agg + outer join with aggregate unsupported",
    16: "COUNT(DISTINCT) and NOT IN subquery unsupported",
    17: "FROM subquery with AVG aggregate unsupported",
    18: "FROM subquery with SUM aggregate unsupported",
    20: "FROM subquery with aggregate unsupported",
    21: "nested EXISTS / NOT EXISTS unsupported",
    22: "EXISTS in target list unsupported",
}


def immv_dir(q: int) -> Path:
    return OUT / f"q{q}" / "postgres" / "immv"


def rel(q: int) -> str:
    return f"accel_tpch_q{q}_immv"


def write_supported(q: int, definition: str) -> None:
    d = immv_dir(q)
    d.mkdir(parents=True, exist_ok=True)
    mv_query = OUT / f"q{q}" / "postgres" / "matview" / "query.sql"
    query_sql = mv_query.read_text(encoding="utf-8")
    query_sql = query_sql.replace(f"accel_tpch_q{q}_matview", rel(q))

    create = (
        f"-- Incrementally maintainable matview (pg_ivm) for TPC-H Q{q}.\n"
        f"-- Same residual grain as matview where possible.\n"
        f"SELECT pgivm.create_immv(\n"
        f"  '{rel(q)}',\n"
        f"  $immv$\n{definition}\n$immv$\n);\n"
    )
    (d / "create.sql").write_text(create, encoding="utf-8")
    (d / "query.sql").write_text(query_sql if query_sql.endswith("\n") else query_sql + "\n", encoding="utf-8")
    (d / "drop.sql").write_text(
        f"DROP TABLE IF EXISTS {rel(q)} CASCADE;\n", encoding="utf-8"
    )
    (d / "status.txt").write_text("supported\n", encoding="utf-8")
    unsupported = d / "unsupported.txt"
    if unsupported.exists():
        unsupported.unlink()


def write_unsupported(q: int, reason: str) -> None:
    d = immv_dir(q)
    d.mkdir(parents=True, exist_ok=True)
    (d / "unsupported.txt").write_text(reason.strip() + "\n", encoding="utf-8")
    (d / "status.txt").write_text(f"unsupported: {reason.strip()}\n", encoding="utf-8")
    (d / "drop.sql").write_text(
        f"DROP TABLE IF EXISTS {rel(q)} CASCADE;\n", encoding="utf-8"
    )


def main() -> None:
    for q, definition in sorted(DEFINITIONS.items()):
        write_supported(q, definition)
        print(f"Q{q}: supported -> {immv_dir(q)}")
    for q, reason in sorted(UNSUPPORTED.items()):
        write_unsupported(q, reason)
        print(f"Q{q}: unsupported ({reason})")


if __name__ == "__main__":
    main()
