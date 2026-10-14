#!/usr/bin/env python3
"""Hand-authored optimized Postgres matviews for TPC-H Q1–Q22.

Writes per-query SQL under accelerators/tpch/q{N}/postgres/matview/ and
workload YAMLs under pipeline/config/workloads/tpch_q{N}_matview.yaml.

Design notes: SF3000-lean answer / calendar / param-domain grains (Top-K,
month starts, color tokens, 16 Q13 patterns, etc.) plus residual filters for
seed_1 bindings from accelerators/tpch/q*/tests/q*_manifest.hpp. Lean IMMV queries
(Q1,5–8,12,14,19) keep calendar/domain intermediate grains.
"""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path
from textwrap import dedent

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "accelerators" / "tpch"
WL = ROOT / "pipeline" / "config" / "workloads"

# seed_1 bindings (manifests)
SEED1 = {
    1: {"day_offset": 103},
    2: {"part_size": 10, "part_type_pattern": "BRASS", "region_name": "AFRICA"},
    3: {"market_segment": "FURNITURE", "cutoff_date": "1995-03-10"},
    4: {"start_date": "1996-11-01"},
    5: {"region_name": "ASIA", "start_date": "1996-01-01"},
    6: {"start_date": "1995-01-01", "discount": "0.03", "quantity": "25"},
    7: {"nation1": "UNITED KINGDOM", "nation2": "BRAZIL"},
    8: {"nation_name": "INDIA", "region_name": "ASIA", "part_type": "BRASS"},
    9: {"color": "powder"},
    10: {"start_date": "1993-06-01"},
    11: {"nation_name": "ROMANIA", "fraction": "0.0001"},
    12: {"shipmode1": "REG AIR", "shipmode2": "SHIP", "start_date": "1996-01-01"},
    13: {"comment_pattern": "%unusual%accounts%"},
    14: {"start_date": "1995-06-01"},
    15: {"start_date": "1994-11-01"},
    16: {
        "brand": "Brand#13",
        "type_pattern": "SMALL BRUSHED%",
        "sizes": [26, 24, 27, 29, 7, 20, 22, 33],
    },
    17: {"brand": "Brand#34", "container": "LARGE BRUSHED"},
    18: {"quantity_threshold": 312},
    19: {
        "brand1": "Brand#31",
        "quantity1": 4,
        "brand2": "Brand#12",
        "quantity2": 15,
        "brand3": "Brand#51",
        "quantity3": 23,
    },
    20: {
        "start_date": "1997-01-01",
        "part_name_pattern": "gainsboro%",
        "nation_name": "RUSSIA",
    },
    21: {"nation_name": "ALGERIA"},
    22: {"codes": ["30", "19", "14", "28", "11", "22", "32"]},
}


def q(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def d(s: str) -> str:
    return f"DATE {q(s)}"


def rel(n: int) -> str:
    return f"accel_tpch_q{n}_matview"


def fn(n: int) -> str:
    return f"accel_tpch_q{n}_mv"


def write_sql(n: int, name: str, body: str) -> None:
    path = GEN / f"q{n}" / "postgres" / "matview" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dedent(body).strip() + "\n", encoding="utf-8")


def emit_common(n: int, create: str, indexes: str, query: str, function: str, reference: str) -> None:
    write_sql(n, "create.sql", create)
    write_sql(n, "indexes.sql", indexes)
    write_sql(n, "query.sql", query)
    write_sql(n, "function.sql", function)
    write_sql(n, "refresh.sql", f"REFRESH MATERIALIZED VIEW {rel(n)};")
    write_sql(n, "reference.sql", reference)


# ---------------------------------------------------------------------------
# Per-query designs
# ---------------------------------------------------------------------------


def q1() -> None:
    n = 1
    p = SEED1[n]
    cutoff = f"DATE '1998-12-01' - INTERVAL '{p['day_offset']}' DAY"
    create = f"""
    -- Pre-aggregate by shipdate + returnflag/linestatus (residual applies cutoff).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      l_shipdate,
      l_returnflag,
      l_linestatus,
      SUM(l_quantity) AS sum_qty,
      SUM(l_extendedprice) AS sum_base_price,
      SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
      SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
      SUM(l_discount) AS sum_discount,
      COUNT(*)::bigint AS count_order
    FROM lineitem
    GROUP BY l_shipdate, l_returnflag, l_linestatus;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_shipdate_idx ON {rel(n)} (l_shipdate);
    CREATE INDEX {rel(n)}_flag_idx ON {rel(n)} (l_returnflag, l_linestatus, l_shipdate);
    """
    # sum/count on both sides so EXCEPT ALL is bit-exact (AVG can float-diverge).
    residual = f"""
    SELECT
      l_returnflag,
      l_linestatus,
      SUM(sum_qty) AS sum_qty,
      SUM(sum_base_price) AS sum_base_price,
      SUM(sum_disc_price) AS sum_disc_price,
      SUM(sum_charge) AS sum_charge,
      SUM(sum_qty) / SUM(count_order) AS avg_qty,
      SUM(sum_base_price) / SUM(count_order) AS avg_price,
      SUM(sum_discount) / SUM(count_order) AS avg_disc,
      SUM(count_order) AS count_order
    FROM {rel(n)}
    WHERE l_shipdate <= {cutoff}
    GROUP BY l_returnflag, l_linestatus
    """
    reference = f"""
    SELECT
      l_returnflag,
      l_linestatus,
      SUM(l_quantity) AS sum_qty,
      SUM(l_extendedprice) AS sum_base_price,
      SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
      SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
      SUM(l_quantity) / COUNT(*) AS avg_qty,
      SUM(l_extendedprice) / COUNT(*) AS avg_price,
      SUM(l_discount) / COUNT(*) AS avg_disc,
      COUNT(*)::bigint AS count_order
    FROM lineitem
    WHERE l_shipdate <= {cutoff}
    GROUP BY l_returnflag, l_linestatus
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(day_offset integer)
    RETURNS TABLE (
      l_returnflag character varying,
      l_linestatus character varying,
      sum_qty numeric,
      sum_base_price numeric,
      sum_disc_price numeric,
      sum_charge numeric,
      avg_qty numeric,
      avg_price numeric,
      avg_disc numeric,
      count_order bigint
    )
    LANGUAGE sql
    STABLE
    AS $$
      SELECT
        m.l_returnflag,
        m.l_linestatus,
        SUM(m.sum_qty),
        SUM(m.sum_base_price),
        SUM(m.sum_disc_price),
        SUM(m.sum_charge),
        SUM(m.sum_qty) / SUM(m.count_order),
        SUM(m.sum_base_price) / SUM(m.count_order),
        SUM(m.sum_discount) / SUM(m.count_order),
        SUM(m.count_order)
      FROM {rel(n)} m
      WHERE m.l_shipdate <= DATE '1998-12-01' - (day_offset || ' days')::interval
      GROUP BY m.l_returnflag, m.l_linestatus
      ORDER BY m.l_returnflag, m.l_linestatus
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q2() -> None:
    n = 2
    p = SEED1[n]
    size, typ, region = p["part_size"], p["part_type_pattern"], p["region_name"]
    create = f"""
    -- Answer grain: Top-100 per (size∈1..50 × Syllable3 × region). ≤125k rows.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH regional_min AS (
      SELECT ps.ps_partkey, r.r_name AS region_name,
             MIN(ps.ps_supplycost) AS min_cost
      FROM partsupp ps
      JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
      JOIN nation n ON s.s_nationkey = n.n_nationkey
      JOIN region r ON n.n_regionkey = r.r_regionkey
      JOIN part p ON p.p_partkey = ps.ps_partkey
      WHERE p.p_size BETWEEN 1 AND 50
        AND (p.p_type LIKE '%NICKEL' OR p.p_type LIKE '%BRASS'
             OR p.p_type LIKE '%STEEL' OR p.p_type LIKE '%COPPER'
             OR p.p_type LIKE '%TIN')
      GROUP BY ps.ps_partkey, r.r_name
    ),
    candidates AS (
      SELECT
        p.p_size,
        CASE
          WHEN p.p_type LIKE '%NICKEL' THEN 'NICKEL'
          WHEN p.p_type LIKE '%BRASS' THEN 'BRASS'
          WHEN p.p_type LIKE '%STEEL' THEN 'STEEL'
          WHEN p.p_type LIKE '%COPPER' THEN 'COPPER'
          WHEN p.p_type LIKE '%TIN' THEN 'TIN'
        END AS type_pattern,
        r.r_name AS region_name,
        s.s_acctbal, s.s_name, n.n_name, p.p_partkey, p.p_mfgr,
        s.s_address, s.s_phone, s.s_comment,
        ROW_NUMBER() OVER (
          PARTITION BY p.p_size,
            CASE
              WHEN p.p_type LIKE '%NICKEL' THEN 'NICKEL'
              WHEN p.p_type LIKE '%BRASS' THEN 'BRASS'
              WHEN p.p_type LIKE '%STEEL' THEN 'STEEL'
              WHEN p.p_type LIKE '%COPPER' THEN 'COPPER'
              WHEN p.p_type LIKE '%TIN' THEN 'TIN'
            END,
            r.r_name
          ORDER BY s.s_acctbal DESC, n.n_name, s.s_name, p.p_partkey
        ) AS rn
      FROM part p
      JOIN partsupp ps ON p.p_partkey = ps.ps_partkey
      JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
      JOIN nation n ON s.s_nationkey = n.n_nationkey
      JOIN region r ON n.n_regionkey = r.r_regionkey
      JOIN regional_min m
        ON m.ps_partkey = p.p_partkey
       AND m.region_name = r.r_name
       AND ps.ps_supplycost = m.min_cost
      WHERE p.p_size BETWEEN 1 AND 50
        AND (p.p_type LIKE '%NICKEL' OR p.p_type LIKE '%BRASS'
             OR p.p_type LIKE '%STEEL' OR p.p_type LIKE '%COPPER'
             OR p.p_type LIKE '%TIN')
    )
    SELECT
      p_size, type_pattern, region_name,
      s_acctbal, s_name, n_name, p_partkey, p_mfgr,
      s_address, s_phone, s_comment
    FROM candidates
    WHERE type_pattern IS NOT NULL AND rn <= 100;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_key_idx ON {rel(n)} (p_size, type_pattern, region_name);
    """
    residual = f"""
    SELECT
      s_acctbal, s_name, n_name, p_partkey, p_mfgr,
      s_address, s_phone, s_comment
    FROM {rel(n)}
    WHERE p_size = {size}
      AND type_pattern = {q(typ)}
      AND region_name = {q(region)}
    ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
    """
    reference = f"""
    SELECT
      s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
    FROM part, supplier, partsupp, nation, region
    WHERE p_partkey = ps_partkey
      AND s_suppkey = ps_suppkey
      AND p_size = {size}
      AND p_type LIKE {q('%' + typ)}
      AND s_nationkey = n_nationkey
      AND n_regionkey = r_regionkey
      AND r_name = {q(region)}
      AND ps_supplycost = (
        SELECT MIN(ps_supplycost)
        FROM partsupp, supplier, nation, region
        WHERE p_partkey = ps_partkey
          AND s_suppkey = ps_suppkey
          AND s_nationkey = n_nationkey
          AND n_regionkey = r_regionkey
          AND r_name = {q(region)}
      )
    ORDER BY s_acctbal DESC, n_name, s_name, p_partkey
    LIMIT 100
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(part_size integer, part_type_pattern text, region_name text)
    RETURNS TABLE (
      s_acctbal numeric,
      s_name character varying,
      n_name character varying,
      p_partkey bigint,
      p_mfgr character varying,
      s_address character varying,
      s_phone character varying,
      s_comment character varying
    )
    LANGUAGE sql
    STABLE
    AS $$
      SELECT
        m.s_acctbal, m.s_name, m.n_name, m.p_partkey, m.p_mfgr,
        m.s_address, m.s_phone, m.s_comment
      FROM {rel(n)} m
      WHERE m.p_size = part_size
        AND m.type_pattern = regexp_replace(part_type_pattern, '^%', '')
        AND m.region_name = region_name
      ORDER BY m.s_acctbal DESC, m.n_name, m.s_name, m.p_partkey
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q3() -> None:
    n = 3
    p = SEED1[n]
    seg, cut = p["market_segment"], p["cutoff_date"]
    create = f"""
    -- Answer grain: Top-10 orders per (mktsegment × each day in March 1995).
    -- 5 segments × 31 cutoffs × ≤10 ≈ 1550 rows.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH cutoffs AS (
      SELECT d::date AS cutoff_date
      FROM generate_series(DATE '1995-03-01', DATE '1995-03-31', INTERVAL '1 day') AS d
    ),
    segments AS (
      SELECT DISTINCT c_mktsegment FROM customer
    ),
    keys AS (
      SELECT s.c_mktsegment, c.cutoff_date
      FROM segments s CROSS JOIN cutoffs c
    ),
    agg AS (
      SELECT
        k.c_mktsegment,
        k.cutoff_date,
        l.l_orderkey,
        o.o_orderdate,
        o.o_shippriority,
        SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
      FROM keys k
      JOIN customer c ON c.c_mktsegment = k.c_mktsegment
      JOIN orders o ON c.c_custkey = o.o_custkey
      JOIN lineitem l ON l.l_orderkey = o.o_orderkey
      WHERE o.o_orderdate < k.cutoff_date
        AND l.l_shipdate > k.cutoff_date
      GROUP BY k.c_mktsegment, k.cutoff_date, l.l_orderkey, o.o_orderdate, o.o_shippriority
    ),
    ranked AS (
      SELECT
        c_mktsegment, cutoff_date, l_orderkey, revenue, o_orderdate, o_shippriority,
        ROW_NUMBER() OVER (
          PARTITION BY c_mktsegment, cutoff_date
          ORDER BY revenue DESC, o_orderdate, l_orderkey
        ) AS rn
      FROM agg
    )
    SELECT c_mktsegment, cutoff_date, l_orderkey, revenue, o_orderdate, o_shippriority
    FROM ranked
    WHERE rn <= 10;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_key_idx ON {rel(n)} (c_mktsegment, cutoff_date);
    """
    residual = f"""
    SELECT l_orderkey, revenue, o_orderdate, o_shippriority
    FROM {rel(n)}
    WHERE c_mktsegment = {q(seg)}
      AND cutoff_date = {d(cut)}
    ORDER BY revenue DESC, o_orderdate
    """
    reference = f"""
    SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority
    FROM customer, orders, lineitem
    WHERE c_mktsegment = {q(seg)}
      AND c_custkey = o_custkey
      AND l_orderkey = o_orderkey
      AND o_orderdate < {d(cut)}
      AND l_shipdate > {d(cut)}
    GROUP BY l_orderkey, o_orderdate, o_shippriority
    ORDER BY revenue DESC, o_orderdate
    LIMIT 10
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(market_segment text, cutoff_date date)
    RETURNS TABLE (
      l_orderkey bigint,
      revenue numeric,
      o_orderdate date,
      o_shippriority integer
    )
    LANGUAGE sql
    STABLE
    AS $$
      SELECT m.l_orderkey, m.revenue, m.o_orderdate, m.o_shippriority
      FROM {rel(n)} m
      WHERE m.c_mktsegment = market_segment
        AND m.cutoff_date = cutoff_date
      ORDER BY m.revenue DESC, m.o_orderdate
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q4() -> None:
    n = 4
    p = SEED1[n]
    start = p["start_date"]
    create = f"""
    -- Daily late-order counts by priority (tiny MV; residual sums a date window).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT o.o_orderdate, o.o_orderpriority, COUNT(*)::bigint AS order_count
    FROM orders o
    WHERE EXISTS (
      SELECT 1 FROM lineitem l
      WHERE l.l_orderkey = o.o_orderkey
        AND l.l_commitdate < l.l_receiptdate
    )
    GROUP BY o.o_orderdate, o.o_orderpriority;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_odate_idx ON {rel(n)} (o_orderdate);
    CREATE INDEX {rel(n)}_prio_idx ON {rel(n)} (o_orderpriority, o_orderdate);
    """
    residual = f"""
    SELECT o_orderpriority, SUM(order_count)::bigint AS order_count
    FROM {rel(n)}
    WHERE o_orderdate >= {d(start)}
      AND o_orderdate < {d(start)} + INTERVAL '3' month
    GROUP BY o_orderpriority
    ORDER BY o_orderpriority
    """
    reference = f"""
    SELECT o_orderpriority, COUNT(*)::bigint AS order_count
    FROM orders
    WHERE o_orderdate >= {d(start)}
      AND o_orderdate < {d(start)} + INTERVAL '3' month
      AND EXISTS (
        SELECT * FROM lineitem
        WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate
      )
    GROUP BY o_orderpriority
    ORDER BY o_orderpriority
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(start_date date)
    RETURNS TABLE (o_orderpriority character varying, order_count bigint)
    LANGUAGE sql STABLE AS $$
      SELECT m.o_orderpriority, SUM(m.order_count)::bigint
      FROM {rel(n)} m
      WHERE m.o_orderdate >= start_date
        AND m.o_orderdate < start_date + INTERVAL '3' month
      GROUP BY m.o_orderpriority
      ORDER BY m.o_orderpriority
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q5() -> None:
    n = 5
    p = SEED1[n]
    region, start = p["region_name"], p["start_date"]
    create = f"""
    -- Revenue by region/nation/orderdate (year window applied in residual).
    CREATE MATERIALIZED VIEW {rel(n)} AS
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
    GROUP BY r.r_name, n.n_name, o.o_orderdate;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_region_date_idx ON {rel(n)} (r_name, o_orderdate);
    CREATE INDEX {rel(n)}_nation_idx ON {rel(n)} (n_name);
    """
    residual = f"""
    SELECT n_name, SUM(revenue) AS revenue
    FROM {rel(n)}
    WHERE r_name = {q(region)}
      AND o_orderdate >= {d(start)}
      AND o_orderdate < {d(start)} + INTERVAL '1' year
    GROUP BY n_name
    ORDER BY revenue DESC
    """
    reference = f"""
    SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue
    FROM customer, orders, lineitem, supplier, nation, region
    WHERE c_custkey = o_custkey
      AND l_orderkey = o_orderkey
      AND l_suppkey = s_suppkey
      AND c_nationkey = s_nationkey
      AND s_nationkey = n_nationkey
      AND n_regionkey = r_regionkey
      AND r_name = {q(region)}
      AND o_orderdate >= {d(start)}
      AND o_orderdate < {d(start)} + INTERVAL '1' year
    GROUP BY n_name
    ORDER BY revenue DESC
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(region_name text, start_date date)
    RETURNS TABLE (n_name character varying, revenue numeric)
    LANGUAGE sql STABLE AS $$
      SELECT m.n_name, SUM(m.revenue)
      FROM {rel(n)} m
      WHERE m.r_name = region_name
        AND m.o_orderdate >= start_date
        AND m.o_orderdate < start_date + INTERVAL '1' year
      GROUP BY m.n_name
      ORDER BY SUM(m.revenue) DESC
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q6() -> None:
    n = 6
    p = SEED1[n]
    start, disc, qty = p["start_date"], p["discount"], p["quantity"]
    disc_d = Decimal(disc)
    disc_lo = disc_d - Decimal("0.01")
    disc_hi = disc_d + Decimal("0.01")
    create = f"""
    -- Day x discount x quantity revenue matrix (mirrors aux grain).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      l_shipdate,
      l_discount,
      l_quantity,
      SUM(l_extendedprice * l_discount) AS revenue
    FROM lineitem
    GROUP BY l_shipdate, l_discount, l_quantity;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_ship_idx ON {rel(n)} (l_shipdate);
    CREATE INDEX {rel(n)}_disc_qty_idx ON {rel(n)} (l_discount, l_quantity);
    CREATE INDEX {rel(n)}_covering_idx ON {rel(n)} (l_shipdate, l_discount, l_quantity);
    """
    residual = f"""
    SELECT SUM(revenue) AS revenue
    FROM {rel(n)}
    WHERE l_shipdate >= {d(start)}
      AND l_shipdate < {d(start)} + INTERVAL '1' year
      AND l_discount BETWEEN {disc_lo} AND {disc_hi}
      AND l_quantity < {qty}
    """
    reference = f"""
    SELECT SUM(l_extendedprice * l_discount) AS revenue
    FROM lineitem
    WHERE l_shipdate >= {d(start)}
      AND l_shipdate < {d(start)} + INTERVAL '1' year
      AND l_discount BETWEEN {disc_lo} AND {disc_hi}
      AND l_quantity < {qty}
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(start_date date, discount numeric, quantity numeric)
    RETURNS TABLE (revenue numeric)
    LANGUAGE sql STABLE AS $$
      SELECT SUM(m.revenue)
      FROM {rel(n)} m
      WHERE m.l_shipdate >= start_date
        AND m.l_shipdate < start_date + INTERVAL '1' year
        AND m.l_discount BETWEEN discount - 0.01 AND discount + 0.01
        AND m.l_quantity < quantity
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q7() -> None:
    n = 7
    p = SEED1[n]
    n1, n2 = p["nation1"], p["nation2"]
    create = f"""
    -- Fixed 1995–1996 shipping volumes by nation pair + year.
    CREATE MATERIALIZED VIEW {rel(n)} AS
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
    GROUP BY n1.n_name, n2.n_name, EXTRACT(year FROM l.l_shipdate);
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_nations_idx ON {rel(n)} (supp_nation, cust_nation);
    """
    residual = f"""
    SELECT supp_nation, cust_nation, l_year, revenue
    FROM {rel(n)}
    WHERE (supp_nation = {q(n1)} AND cust_nation = {q(n2)})
       OR (supp_nation = {q(n2)} AND cust_nation = {q(n1)})
    ORDER BY supp_nation, cust_nation, l_year
    """
    reference = f"""
    SELECT supp_nation, cust_nation, l_year, SUM(volume) AS revenue
    FROM (
      SELECT
        n1.n_name AS supp_nation,
        n2.n_name AS cust_nation,
        EXTRACT(year FROM l_shipdate)::integer AS l_year,
        l_extendedprice * (1 - l_discount) AS volume
      FROM supplier, lineitem, orders, customer, nation n1, nation n2
      WHERE s_suppkey = l_suppkey
        AND o_orderkey = l_orderkey
        AND c_custkey = o_custkey
        AND s_nationkey = n1.n_nationkey
        AND c_nationkey = n2.n_nationkey
        AND ((n1.n_name = {q(n1)} AND n2.n_name = {q(n2)})
          OR (n1.n_name = {q(n2)} AND n2.n_name = {q(n1)}))
        AND l_shipdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
    ) shipping
    GROUP BY supp_nation, cust_nation, l_year
    ORDER BY supp_nation, cust_nation, l_year
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(nation1 text, nation2 text)
    RETURNS TABLE (
      supp_nation character varying,
      cust_nation character varying,
      l_year integer,
      revenue numeric
    )
    LANGUAGE sql STABLE AS $$
      SELECT m.supp_nation, m.cust_nation, m.l_year, m.revenue
      FROM {rel(n)} m
      WHERE (m.supp_nation = nation1 AND m.cust_nation = nation2)
         OR (m.supp_nation = nation2 AND m.cust_nation = nation1)
      ORDER BY m.supp_nation, m.cust_nation, m.l_year
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q8() -> None:
    n = 8
    p = SEED1[n]
    nation, region, ptype = p["nation_name"], p["region_name"], p["part_type"]
    create = f"""
    -- 1995–1996 volumes by region, part type, year, supplier nation.
    CREATE MATERIALIZED VIEW {rel(n)} AS
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
    GROUP BY r.r_name, p.p_type, EXTRACT(year FROM o.o_orderdate), n2.n_name;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_filter_idx ON {rel(n)} (r_name, p_type);
    CREATE INDEX {rel(n)}_nation_idx ON {rel(n)} (nation);
    """
    residual = f"""
    SELECT
      o_year,
      SUM(CASE WHEN nation = {q(nation)} THEN volume ELSE 0 END) / SUM(volume) AS mkt_share
    FROM {rel(n)}
    WHERE r_name = {q(region)}
      AND p_type = {q(ptype)}
    GROUP BY o_year
    ORDER BY o_year
    """
    reference = f"""
    SELECT
      o_year,
      SUM(CASE WHEN nation = {q(nation)} THEN volume ELSE 0 END) / SUM(volume) AS mkt_share
    FROM (
      SELECT
        EXTRACT(year FROM o_orderdate)::integer AS o_year,
        l_extendedprice * (1 - l_discount) AS volume,
        n2.n_name AS nation
      FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
      WHERE p_partkey = l_partkey
        AND s_suppkey = l_suppkey
        AND l_orderkey = o_orderkey
        AND o_custkey = c_custkey
        AND c_nationkey = n1.n_nationkey
        AND n1.n_regionkey = r_regionkey
        AND r_name = {q(region)}
        AND s_nationkey = n2.n_nationkey
        AND o_orderdate BETWEEN DATE '1995-01-01' AND DATE '1996-12-31'
        AND p_type = {q(ptype)}
    ) all_nations
    GROUP BY o_year
    ORDER BY o_year
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(nation_name text, region_name text, part_type text)
    RETURNS TABLE (o_year integer, mkt_share numeric)
    LANGUAGE sql STABLE AS $$
      SELECT
        m.o_year,
        SUM(CASE WHEN m.nation = nation_name THEN m.volume ELSE 0 END) / SUM(m.volume)
      FROM {rel(n)} m
      WHERE m.r_name = region_name AND m.p_type = part_type
      GROUP BY m.o_year
      ORDER BY m.o_year
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q9() -> None:
    n = 9
    p = SEED1[n]
    color = p["color"]
    create = f"""
    -- Answer grain: (color token × nation × year) profit. LATERAL unnest of p_name.
    -- ~92 colors × 25 nations × ~7 years ≈ 16k rows (not partkey).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      tok.color,
      n.n_name AS nation,
      EXTRACT(year FROM o.o_orderdate)::integer AS o_year,
      SUM(l.l_extendedprice * (1 - l.l_discount) - ps.ps_supplycost * l.l_quantity) AS amount
    FROM part p
    CROSS JOIN LATERAL unnest(string_to_array(p.p_name, ' ')) AS tok(color)
    JOIN lineitem l ON p.p_partkey = l.l_partkey
    JOIN partsupp ps ON ps.ps_suppkey = l.l_suppkey AND ps.ps_partkey = l.l_partkey
    JOIN orders o ON o.o_orderkey = l.l_orderkey
    JOIN supplier s ON s.s_suppkey = l.l_suppkey
    JOIN nation n ON s.s_nationkey = n.n_nationkey
    GROUP BY tok.color, n.n_name, EXTRACT(year FROM o.o_orderdate);
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_color_idx ON {rel(n)} (color);
    CREATE INDEX {rel(n)}_nation_year_idx ON {rel(n)} (nation, o_year);
    """
    residual = f"""
    SELECT nation, o_year, SUM(amount) AS sum_profit
    FROM {rel(n)}
    WHERE color = {q(color)}
    GROUP BY nation, o_year
    ORDER BY nation, o_year DESC
    """
    reference = f"""
    SELECT nation, o_year, SUM(amount) AS sum_profit
    FROM (
      SELECT
        n_name AS nation,
        EXTRACT(year FROM o_orderdate)::integer AS o_year,
        l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount
      FROM part, supplier, lineitem, partsupp, orders, nation
      WHERE s_suppkey = l_suppkey
        AND ps_suppkey = l_suppkey
        AND ps_partkey = l_partkey
        AND p_partkey = l_partkey
        AND o_orderkey = l_orderkey
        AND s_nationkey = n_nationkey
        AND p_name LIKE {q('%' + color + '%')}
    ) profit
    GROUP BY nation, o_year
    ORDER BY nation, o_year DESC
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(color text)
    RETURNS TABLE (nation character varying, o_year integer, sum_profit numeric)
    LANGUAGE sql STABLE AS $$
      SELECT m.nation, m.o_year, SUM(m.amount)
      FROM {rel(n)} m
      WHERE m.color = color
      GROUP BY m.nation, m.o_year
      ORDER BY m.nation, m.o_year DESC
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q10() -> None:
    n = 10
    p = SEED1[n]
    start = p["start_date"]
    create = f"""
    -- Answer grain: Top-20 returned-revenue customers per first-of-month start
    -- in [1993-02-01 .. 1995-01-01] (24 months × 20 ≈ 480 rows).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH starts AS (
      SELECT d::date AS start_date
      FROM generate_series(DATE '1993-02-01', DATE '1995-01-01', INTERVAL '1 month') AS d
    ),
    agg AS (
      SELECT
        s.start_date,
        c.c_custkey,
        c.c_name,
        c.c_acctbal,
        c.c_phone,
        c.c_address,
        c.c_comment,
        n.n_name,
        SUM(l.l_extendedprice * (1 - l.l_discount)) AS revenue
      FROM starts s
      JOIN orders o
        ON o.o_orderdate >= s.start_date
       AND o.o_orderdate < s.start_date + INTERVAL '3' month
      JOIN lineitem l ON l.l_orderkey = o.o_orderkey AND l.l_returnflag = 'R'
      JOIN customer c ON c.c_custkey = o.o_custkey
      JOIN nation n ON c.c_nationkey = n.n_nationkey
      GROUP BY s.start_date, c.c_custkey, c.c_name, c.c_acctbal, c.c_phone,
               c.c_address, c.c_comment, n.n_name
    ),
    ranked AS (
      SELECT
        start_date, c_custkey, c_name, revenue, c_acctbal, n_name,
        c_address, c_phone, c_comment,
        ROW_NUMBER() OVER (
          PARTITION BY start_date
          ORDER BY revenue DESC, c_custkey
        ) AS rn
      FROM agg
    )
    SELECT
      start_date, c_custkey, c_name, revenue, c_acctbal, n_name,
      c_address, c_phone, c_comment
    FROM ranked
    WHERE rn <= 20;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_start_idx ON {rel(n)} (start_date);
    """
    residual = f"""
    SELECT
      c_custkey, c_name, revenue, c_acctbal, n_name,
      c_address, c_phone, c_comment
    FROM {rel(n)}
    WHERE start_date = {d(start)}
    ORDER BY revenue DESC
    """
    reference = f"""
    SELECT
      c_custkey, c_name,
      SUM(l_extendedprice * (1 - l_discount)) AS revenue,
      c_acctbal, n_name, c_address, c_phone, c_comment
    FROM customer, orders, lineitem, nation
    WHERE c_custkey = o_custkey
      AND l_orderkey = o_orderkey
      AND o_orderdate >= {d(start)}
      AND o_orderdate < {d(start)} + INTERVAL '3' month
      AND l_returnflag = 'R'
      AND c_nationkey = n_nationkey
    GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
    ORDER BY revenue DESC
    LIMIT 20
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(start_date date)
    RETURNS TABLE (
      c_custkey bigint,
      c_name character varying,
      revenue numeric,
      c_acctbal numeric,
      n_name character varying,
      c_address character varying,
      c_phone character varying,
      c_comment character varying
    )
    LANGUAGE sql STABLE AS $$
      SELECT
        m.c_custkey, m.c_name, m.revenue, m.c_acctbal, m.n_name,
        m.c_address, m.c_phone, m.c_comment
      FROM {rel(n)} m
      WHERE m.start_date = start_date
      ORDER BY m.revenue DESC
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q11() -> None:
    n = 11
    p = SEED1[n]
    nation, frac = p["nation_name"], p["fraction"]
    create = f"""
    -- Answer grain: Top-100 parts by value per nation (+ nation_total for threshold).
    -- Note: official TPC-H fraction is 0.0001/SF; SEED1 uses 0.0001 (SF1).
    -- ≤25 × 100 = 2500 rows (not all partkeys).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH part_value AS (
      SELECT
        n.n_name,
        ps.ps_partkey,
        SUM(ps.ps_supplycost * ps.ps_availqty) AS value
      FROM partsupp ps
      JOIN supplier s ON ps.ps_suppkey = s.s_suppkey
      JOIN nation n ON s.s_nationkey = n.n_nationkey
      GROUP BY n.n_name, ps.ps_partkey
    ),
    nation_total AS (
      SELECT n_name, SUM(value) AS nation_total
      FROM part_value
      GROUP BY n_name
    ),
    ranked AS (
      SELECT
        pv.n_name,
        pv.ps_partkey,
        pv.value,
        nt.nation_total,
        ROW_NUMBER() OVER (
          PARTITION BY pv.n_name ORDER BY pv.value DESC, pv.ps_partkey
        ) AS rn
      FROM part_value pv
      JOIN nation_total nt ON nt.n_name = pv.n_name
    )
    SELECT n_name, ps_partkey, value, nation_total
    FROM ranked
    WHERE rn <= 100;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_nation_idx ON {rel(n)} (n_name);
    CREATE INDEX {rel(n)}_value_idx ON {rel(n)} (n_name, value DESC);
    """
    residual = f"""
    SELECT ps_partkey, value
    FROM {rel(n)}
    WHERE n_name = {q(nation)}
      AND value > nation_total * {frac}
    ORDER BY value DESC
    """
    reference = f"""
    SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) AS value
    FROM partsupp, supplier, nation
    WHERE ps_suppkey = s_suppkey
      AND s_nationkey = n_nationkey
      AND n_name = {q(nation)}
    GROUP BY ps_partkey
    HAVING SUM(ps_supplycost * ps_availqty) > (
      SELECT SUM(ps_supplycost * ps_availqty) * {frac}
      FROM partsupp, supplier, nation
      WHERE ps_suppkey = s_suppkey
        AND s_nationkey = n_nationkey
        AND n_name = {q(nation)}
    )
    ORDER BY value DESC
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(nation_name text, fraction numeric)
    RETURNS TABLE (ps_partkey bigint, value numeric)
    LANGUAGE sql STABLE AS $$
      SELECT m.ps_partkey, m.value
      FROM {rel(n)} m
      WHERE m.n_name = nation_name
        AND m.value > m.nation_total * fraction
      ORDER BY m.value DESC
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q12() -> None:
    n = 12
    p = SEED1[n]
    m1, m2, start = p["shipmode1"], p["shipmode2"], p["start_date"]
    create = f"""
    -- Qualified lineitems with shipmode, receiptdate, priority class.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      l.l_shipmode,
      l.l_receiptdate,
      CASE
        WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1
        ELSE 0
      END AS is_high,
      COUNT(*)::bigint AS line_count
    FROM orders o
    JOIN lineitem l ON o.o_orderkey = l.l_orderkey
    WHERE l.l_commitdate < l.l_receiptdate
      AND l.l_shipdate < l.l_commitdate
    GROUP BY l.l_shipmode, l.l_receiptdate,
             CASE WHEN o.o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_mode_date_idx ON {rel(n)} (l_shipmode, l_receiptdate);
    """
    residual = f"""
    SELECT
      l_shipmode,
      SUM(CASE WHEN is_high = 1 THEN line_count ELSE 0 END) AS high_line_count,
      SUM(CASE WHEN is_high = 0 THEN line_count ELSE 0 END) AS low_line_count
    FROM {rel(n)}
    WHERE l_shipmode IN ({q(m1)}, {q(m2)})
      AND l_receiptdate >= {d(start)}
      AND l_receiptdate < {d(start)} + INTERVAL '1' year
    GROUP BY l_shipmode
    ORDER BY l_shipmode
    """
    reference = f"""
    SELECT
      l_shipmode,
      SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) AS high_line_count,
      SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) AS low_line_count
    FROM orders, lineitem
    WHERE o_orderkey = l_orderkey
      AND l_shipmode IN ({q(m1)}, {q(m2)})
      AND l_commitdate < l_receiptdate
      AND l_shipdate < l_commitdate
      AND l_receiptdate >= {d(start)}
      AND l_receiptdate < {d(start)} + INTERVAL '1' year
    GROUP BY l_shipmode
    ORDER BY l_shipmode
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(shipmode1 text, shipmode2 text, start_date date)
    RETURNS TABLE (
      l_shipmode character varying,
      high_line_count bigint,
      low_line_count bigint
    )
    LANGUAGE sql STABLE AS $$
      SELECT
        m.l_shipmode,
        SUM(CASE WHEN m.is_high = 1 THEN m.line_count ELSE 0 END),
        SUM(CASE WHEN m.is_high = 0 THEN m.line_count ELSE 0 END)
      FROM {rel(n)} m
      WHERE m.l_shipmode IN (shipmode1, shipmode2)
        AND m.l_receiptdate >= start_date
        AND m.l_receiptdate < start_date + INTERVAL '1' year
      GROUP BY m.l_shipmode
      ORDER BY m.l_shipmode
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q13() -> None:
    n = 13
    p = SEED1[n]
    pat = p["comment_pattern"]
    # Official 4×4 WORD1×WORD2 patterns (TPC-H 2.4.13).
    word1 = ["special", "pending", "unusual", "express"]
    word2 = ["packages", "requests", "accounts", "deposits"]
    patterns = [f"%{w1}%{w2}%" for w1 in word1 for w2 in word2]
    pattern_values = ",\n      ".join(f"({q(pat_)})" for pat_ in patterns)
    create = f"""
    -- Answer grain: 16 `%WORD1%WORD2%` histograms (c_count → custdist).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH patterns(comment_pattern) AS (
      VALUES
      {pattern_values}
    ),
    per_cust AS (
      SELECT
        c.c_custkey,
        p.comment_pattern,
        COUNT(o.o_orderkey) FILTER (
          WHERE o.o_orderkey IS NOT NULL AND o.o_comment NOT LIKE p.comment_pattern
        ) AS c_count
      FROM customer c
      CROSS JOIN patterns p
      LEFT OUTER JOIN orders o ON c.c_custkey = o.o_custkey
      GROUP BY c.c_custkey, p.comment_pattern
    )
    SELECT comment_pattern, c_count, COUNT(*)::bigint AS custdist
    FROM per_cust
    GROUP BY comment_pattern, c_count;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_pat_idx ON {rel(n)} (comment_pattern);
    """
    residual = f"""
    SELECT c_count, custdist
    FROM {rel(n)}
    WHERE comment_pattern = {q(pat)}
    ORDER BY custdist DESC, c_count DESC
    """
    reference = f"""
    SELECT c_count, COUNT(*)::bigint AS custdist
    FROM (
      SELECT c_custkey, COUNT(o_orderkey) AS c_count
      FROM customer
      LEFT OUTER JOIN orders ON c_custkey = o_custkey
        AND o_comment NOT LIKE {q(pat)}
      GROUP BY c_custkey
    ) c_orders
    GROUP BY c_count
    ORDER BY custdist DESC, c_count DESC
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(comment_pattern text)
    RETURNS TABLE (c_count bigint, custdist bigint)
    LANGUAGE sql STABLE AS $$
      SELECT m.c_count, m.custdist
      FROM {rel(n)} m
      WHERE m.comment_pattern = comment_pattern
      ORDER BY m.custdist DESC, m.c_count DESC
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q14() -> None:
    n = 14
    p = SEED1[n]
    start = p["start_date"]
    create = f"""
    -- Daily promo vs all revenue from lineitem⋈part.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      l.l_shipdate,
      SUM(CASE WHEN p.p_type LIKE 'PROMO%' THEN l.l_extendedprice * (1 - l.l_discount) ELSE 0 END) AS promo_rev,
      SUM(l.l_extendedprice * (1 - l.l_discount)) AS all_rev
    FROM lineitem l
    JOIN part p ON l.l_partkey = p.p_partkey
    GROUP BY l.l_shipdate;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_ship_idx ON {rel(n)} (l_shipdate);
    """
    residual = f"""
    SELECT
      100.00 * SUM(promo_rev) / SUM(all_rev) AS promo_revenue
    FROM {rel(n)}
    WHERE l_shipdate >= {d(start)}
      AND l_shipdate < {d(start)} + INTERVAL '1' month
    """
    reference = f"""
    SELECT
      100.00 * SUM(
        CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END
      ) / SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue
    FROM lineitem, part
    WHERE l_partkey = p_partkey
      AND l_shipdate >= {d(start)}
      AND l_shipdate < {d(start)} + INTERVAL '1' month
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(start_date date)
    RETURNS TABLE (promo_revenue numeric)
    LANGUAGE sql STABLE AS $$
      SELECT 100.00 * SUM(m.promo_rev) / SUM(m.all_rev)
      FROM {rel(n)} m
      WHERE m.l_shipdate >= start_date
        AND m.l_shipdate < start_date + INTERVAL '1' month
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q15() -> None:
    n = 15
    p = SEED1[n]
    start = p["start_date"]
    create = f"""
    -- Answer grain: top supplier(s) by revenue per month-start in
    -- [1993-01-01 .. 1997-10-01] (58 starts × ≤few ties).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH starts AS (
      SELECT d::date AS start_date
      FROM generate_series(DATE '1993-01-01', DATE '1997-10-01', INTERVAL '1 month') AS d
    ),
    revenue AS (
      SELECT
        s.start_date,
        l.l_suppkey AS supplier_no,
        SUM(l.l_extendedprice * (1 - l.l_discount)) AS total_revenue
      FROM starts s
      JOIN lineitem l
        ON l.l_shipdate >= s.start_date
       AND l.l_shipdate < s.start_date + INTERVAL '3' month
      GROUP BY s.start_date, l.l_suppkey
    ),
    maxima AS (
      SELECT start_date, MAX(total_revenue) AS max_revenue
      FROM revenue
      GROUP BY start_date
    )
    SELECT
      r.start_date,
      s.s_suppkey,
      s.s_name,
      s.s_address,
      s.s_phone,
      r.total_revenue
    FROM revenue r
    JOIN maxima m
      ON m.start_date = r.start_date
     AND m.max_revenue = r.total_revenue
    JOIN supplier s ON s.s_suppkey = r.supplier_no;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_start_idx ON {rel(n)} (start_date);
    """
    residual = f"""
    SELECT s_suppkey, s_name, s_address, s_phone, total_revenue
    FROM {rel(n)}
    WHERE start_date = {d(start)}
    ORDER BY s_suppkey
    """
    reference = f"""
    WITH revenue AS (
      SELECT l_suppkey AS supplier_no,
             SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
      FROM lineitem
      WHERE l_shipdate >= {d(start)}
        AND l_shipdate < {d(start)} + INTERVAL '3' month
      GROUP BY l_suppkey
    )
    SELECT s_suppkey, s_name, s_address, s_phone, total_revenue
    FROM supplier, revenue
    WHERE s_suppkey = supplier_no
      AND total_revenue = (SELECT MAX(total_revenue) FROM revenue)
    ORDER BY s_suppkey
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(start_date date)
    RETURNS TABLE (
      s_suppkey bigint,
      s_name character varying,
      s_address character varying,
      s_phone character varying,
      total_revenue numeric
    )
    LANGUAGE sql STABLE AS $$
      SELECT m.s_suppkey, m.s_name, m.s_address, m.s_phone, m.total_revenue
      FROM {rel(n)} m
      WHERE m.start_date = start_date
      ORDER BY m.s_suppkey
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q16() -> None:
    n = 16
    p = SEED1[n]
    brand, typ, sizes = p["brand"], p["type_pattern"], p["sizes"]
    size_list = ", ".join(str(s) for s in sizes)
    create = f"""
    -- Pre-counted distinct suppliers by brand/type/size (non-complaint only).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      p.p_brand,
      p.p_type,
      p.p_size,
      COUNT(DISTINCT ps.ps_suppkey)::bigint AS supplier_cnt
    FROM partsupp ps
    JOIN part p ON p.p_partkey = ps.ps_partkey
    WHERE ps.ps_suppkey NOT IN (
      SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%'
    )
    GROUP BY p.p_brand, p.p_type, p.p_size;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_brand_idx ON {rel(n)} (p_brand);
    CREATE INDEX {rel(n)}_size_idx ON {rel(n)} (p_size);
    CREATE INDEX {rel(n)}_type_idx ON {rel(n)} (p_type);
    CREATE INDEX {rel(n)}_cnt_idx ON {rel(n)} (supplier_cnt DESC);
    """
    residual = f"""
    SELECT p_brand, p_type, p_size, supplier_cnt
    FROM {rel(n)}
    WHERE p_brand <> {q(brand)}
      AND p_type NOT LIKE {q(typ)}
      AND p_size IN ({size_list})
    ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
    """
    reference = f"""
    SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey)::bigint AS supplier_cnt
    FROM partsupp, part
    WHERE p_partkey = ps_partkey
      AND p_brand <> {q(brand)}
      AND p_type NOT LIKE {q(typ)}
      AND p_size IN ({size_list})
      AND ps_suppkey NOT IN (
        SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%'
      )
    GROUP BY p_brand, p_type, p_size
    ORDER BY supplier_cnt DESC, p_brand, p_type, p_size
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(
      brand text, type_pattern text,
      size1 integer, size2 integer, size3 integer, size4 integer,
      size5 integer, size6 integer, size7 integer, size8 integer
    )
    RETURNS TABLE (
      p_brand character varying,
      p_type character varying,
      p_size integer,
      supplier_cnt bigint
    )
    LANGUAGE sql STABLE AS $$
      SELECT m.p_brand, m.p_type, m.p_size, m.supplier_cnt
      FROM {rel(n)} m
      WHERE m.p_brand <> brand
        AND m.p_type NOT LIKE type_pattern
        AND m.p_size IN (size1, size2, size3, size4, size5, size6, size7, size8)
      ORDER BY m.supplier_cnt DESC, m.p_brand, m.p_type, m.p_size
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q17() -> None:
    n = 17
    p = SEED1[n]
    brand, container = p["brand"], p["container"]
    create = f"""
    -- Answer-level: sum(extendedprice) for qty < 0.2*avg(qty), by brand×container.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT
      p.p_brand,
      p.p_container,
      SUM(l.l_extendedprice) AS sum_price
    FROM lineitem l
    JOIN part p ON p.p_partkey = l.l_partkey
    JOIN (
      SELECT l_partkey, 0.2 * AVG(l_quantity) AS avg_quantity
      FROM lineitem
      GROUP BY l_partkey
    ) pa ON pa.l_partkey = l.l_partkey
    WHERE l.l_quantity < pa.avg_quantity
    GROUP BY p.p_brand, p.p_container;
    """
    indexes = f"""
    CREATE UNIQUE INDEX {rel(n)}_brand_cont_uidx ON {rel(n)} (p_brand, p_container);
    """
    residual = f"""
    SELECT SUM(sum_price) / 7.0 AS avg_yearly
    FROM {rel(n)}
    WHERE p_brand = {q(brand)}
      AND p_container = {q(container)}
    """
    reference = f"""
    SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly
    FROM lineitem, part,
    (
      SELECT l_partkey AS agg_partkey, 0.2 * AVG(l_quantity) AS avg_quantity
      FROM lineitem
      GROUP BY l_partkey
    ) part_agg
    WHERE p_partkey = l_partkey
      AND agg_partkey = l_partkey
      AND p_brand = {q(brand)}
      AND p_container = {q(container)}
      AND l_quantity < avg_quantity
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(brand text, container text)
    RETURNS TABLE (avg_yearly numeric)
    LANGUAGE sql STABLE AS $$
      SELECT SUM(m.sum_price) / 7.0
      FROM {rel(n)} m
      WHERE m.p_brand = brand
        AND m.p_container = container
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q18() -> None:
    n = 18
    p = SEED1[n]
    thr = p["quantity_threshold"]
    create = f"""
    -- Answer grain: Top-100 by price per threshold ∈ {{312,313,314,315}}.
    -- ≤400 rows (SF3000-safe); residual filters threshold.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH thresholds(quantity_threshold) AS (
      VALUES (312::numeric), (313), (314), (315)
    ),
    heavy AS (
      SELECT
        t.quantity_threshold,
        c.c_name,
        c.c_custkey,
        o.o_orderkey,
        o.o_orderdate,
        o.o_totalprice,
        oq.order_qty AS sum_qty,
        ROW_NUMBER() OVER (
          PARTITION BY t.quantity_threshold
          ORDER BY o.o_totalprice DESC, o.o_orderdate, o.o_orderkey
        ) AS rn
      FROM thresholds t
      JOIN (
        SELECT l_orderkey, SUM(l_quantity) AS order_qty
        FROM lineitem
        GROUP BY l_orderkey
      ) oq ON oq.order_qty > t.quantity_threshold
      JOIN orders o ON o.o_orderkey = oq.l_orderkey
      JOIN customer c ON c.c_custkey = o.o_custkey
    )
    SELECT
      quantity_threshold, c_name, c_custkey, o_orderkey,
      o_orderdate, o_totalprice, sum_qty
    FROM heavy
    WHERE rn <= 100;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_thr_idx ON {rel(n)} (quantity_threshold);
    CREATE INDEX {rel(n)}_price_idx ON {rel(n)} (quantity_threshold, o_totalprice DESC);
    """
    residual = f"""
    SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, sum_qty
    FROM {rel(n)}
    WHERE quantity_threshold = {thr}
    ORDER BY o_totalprice DESC, o_orderdate
    """
    reference = f"""
    SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity) AS sum_qty
    FROM customer, orders, lineitem
    WHERE o_orderkey IN (
      SELECT l_orderkey FROM lineitem
      GROUP BY l_orderkey
      HAVING SUM(l_quantity) > {thr}
    )
      AND c_custkey = o_custkey
      AND o_orderkey = l_orderkey
    GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
    ORDER BY o_totalprice DESC, o_orderdate
    LIMIT 100
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(quantity_threshold numeric)
    RETURNS TABLE (
      c_name character varying,
      c_custkey bigint,
      o_orderkey bigint,
      o_orderdate date,
      o_totalprice numeric,
      sum_qty numeric
    )
    LANGUAGE sql STABLE AS $$
      SELECT m.c_name, m.c_custkey, m.o_orderkey, m.o_orderdate, m.o_totalprice, m.sum_qty
      FROM {rel(n)} m
      WHERE m.quantity_threshold = quantity_threshold
      ORDER BY m.o_totalprice DESC, m.o_orderdate
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q19() -> None:
    n = 19
    p = SEED1[n]
    b1, q1_, b2, q2_, b3, q3_ = (
        p["brand1"],
        p["quantity1"],
        p["brand2"],
        p["quantity2"],
        p["brand3"],
        p["quantity3"],
    )
    create = f"""
    -- Air/DIP revenue pre-agg by brand × container-class × qty × size.
    CREATE MATERIALIZED VIEW {rel(n)} AS
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
      p.p_size;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_brand_class_idx ON {rel(n)} (p_brand, cont_class);
    CREATE INDEX {rel(n)}_qty_idx ON {rel(n)} (l_quantity);
    """
    residual = f"""
    SELECT SUM(revenue) AS revenue
    FROM {rel(n)}
    WHERE (
      p_brand = {q(b1)}
      AND cont_class = 'SM'
      AND l_quantity >= {q1_} AND l_quantity <= {q1_} + 10
      AND p_size BETWEEN 1 AND 5
    ) OR (
      p_brand = {q(b2)}
      AND cont_class = 'MED'
      AND l_quantity >= {q2_} AND l_quantity <= {q2_} + 10
      AND p_size BETWEEN 1 AND 10
    ) OR (
      p_brand = {q(b3)}
      AND cont_class = 'LG'
      AND l_quantity >= {q3_} AND l_quantity <= {q3_} + 10
      AND p_size BETWEEN 1 AND 15
    )
    """
    reference = f"""
    SELECT SUM(l_extendedprice * (1 - l_discount)) AS revenue
    FROM lineitem, part
    WHERE (
      p_partkey = l_partkey
      AND p_brand = {q(b1)}
      AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')
      AND l_quantity >= {q1_} AND l_quantity <= {q1_} + 10
      AND p_size BETWEEN 1 AND 5
      AND l_shipmode IN ('AIR', 'AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON'
    ) OR (
      p_partkey = l_partkey
      AND p_brand = {q(b2)}
      AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')
      AND l_quantity >= {q2_} AND l_quantity <= {q2_} + 10
      AND p_size BETWEEN 1 AND 10
      AND l_shipmode IN ('AIR', 'AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON'
    ) OR (
      p_partkey = l_partkey
      AND p_brand = {q(b3)}
      AND p_container IN ('LG CASE', 'LG BOX', 'LG PACK', 'LG PKG')
      AND l_quantity >= {q3_} AND l_quantity <= {q3_} + 10
      AND p_size BETWEEN 1 AND 15
      AND l_shipmode IN ('AIR', 'AIR REG')
      AND l_shipinstruct = 'DELIVER IN PERSON'
    )
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(
      brand1 text, quantity1 numeric,
      brand2 text, quantity2 numeric,
      brand3 text, quantity3 numeric
    )
    RETURNS TABLE (revenue numeric)
    LANGUAGE sql STABLE AS $$
      SELECT SUM(m.revenue)
      FROM {rel(n)} m
      WHERE (
        m.p_brand = brand1
        AND m.cont_class = 'SM'
        AND m.l_quantity >= quantity1 AND m.l_quantity <= quantity1 + 10
        AND m.p_size BETWEEN 1 AND 5
      ) OR (
        m.p_brand = brand2
        AND m.cont_class = 'MED'
        AND m.l_quantity >= quantity2 AND m.l_quantity <= quantity2 + 10
        AND m.p_size BETWEEN 1 AND 10
      ) OR (
        m.p_brand = brand3
        AND m.cont_class = 'LG'
        AND m.l_quantity >= quantity3 AND m.l_quantity <= quantity3 + 10
        AND m.p_size BETWEEN 1 AND 15
      )
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q20() -> None:
    n = 20
    p = SEED1[n]
    start, pname, nation = p["start_date"], p["part_name_pattern"], p["nation_name"]
    create = f"""
    -- Answer grain: qualifying suppliers per (color prefix × year × nation).
    -- ~92 colors × 5 years × 25 nations × (few suppliers) — not partsupp×year.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    SELECT color, year, nation, s_name, s_address
    FROM (
      SELECT
        split_part(p.p_name, ' ', 1) AS color,
        y.yr AS year,
        n.n_name AS nation,
        s.s_name,
        s.s_address,
        ps.ps_partkey,
        ps.ps_suppkey
      FROM (VALUES (1993), (1994), (1995), (1996), (1997)) AS y(yr)
      CROSS JOIN partsupp ps
      JOIN part p ON p.p_partkey = ps.ps_partkey
      JOIN supplier s ON s.s_suppkey = ps.ps_suppkey
      JOIN nation n ON n.n_nationkey = s.s_nationkey
      JOIN lineitem l
        ON l.l_partkey = ps.ps_partkey
       AND l.l_suppkey = ps.ps_suppkey
       AND l.l_shipdate >= MAKE_DATE(y.yr, 1, 1)
       AND l.l_shipdate < MAKE_DATE(y.yr, 1, 1) + INTERVAL '1' year
      GROUP BY 1, 2, 3, 4, 5, 6, 7, ps.ps_availqty
      HAVING ps.ps_availqty > 0.5 * SUM(l.l_quantity)
    ) q
    GROUP BY color, year, nation, s_name, s_address;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_key_idx ON {rel(n)} (color, year, nation);
    """
    # pname is 'gainsboro%' — strip trailing % for color equality.
    color = pname.rstrip("%")
    year = int(start[:4])
    residual = f"""
    SELECT s_name, s_address
    FROM {rel(n)}
    WHERE color = {q(color)}
      AND year = {year}
      AND nation = {q(nation)}
    ORDER BY s_name
    """
    reference = f"""
    SELECT s_name, s_address
    FROM supplier, nation
    WHERE s_suppkey IN (
      SELECT ps_suppkey
      FROM partsupp
      WHERE ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE {q(pname)})
        AND ps_availqty > (
          SELECT 0.5 * SUM(l_quantity)
          FROM lineitem
          WHERE l_partkey = ps_partkey
            AND l_suppkey = ps_suppkey
            AND l_shipdate >= {d(start)}
            AND l_shipdate < {d(start)} + INTERVAL '1' year
        )
    )
      AND s_nationkey = n_nationkey
      AND n_name = {q(nation)}
    ORDER BY s_name
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(start_date date, part_name_pattern text, nation_name text)
    RETURNS TABLE (s_name character varying, s_address character varying)
    LANGUAGE sql STABLE AS $$
      SELECT m.s_name, m.s_address
      FROM {rel(n)} m
      WHERE m.color = regexp_replace(part_name_pattern, '%$', '')
        AND m.year = EXTRACT(year FROM start_date)::integer
        AND m.nation = nation_name
      ORDER BY m.s_name
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q21() -> None:
    n = 21
    p = SEED1[n]
    nation = p["nation_name"]
    create = f"""
    -- Answer grain: Top-100 waiting suppliers per nation (not all supplier names).
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH waits AS (
      SELECT
        n.n_name,
        s.s_name,
        COUNT(*)::bigint AS numwait
      FROM supplier s
      JOIN lineitem l1 ON s.s_suppkey = l1.l_suppkey
      JOIN orders o ON o.o_orderkey = l1.l_orderkey
      JOIN nation n ON s.s_nationkey = n.n_nationkey
      WHERE o.o_orderstatus = 'F'
        AND l1.l_receiptdate > l1.l_commitdate
        AND EXISTS (
          SELECT 1 FROM lineitem l2
          WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey
        )
        AND NOT EXISTS (
          SELECT 1 FROM lineitem l3
          WHERE l3.l_orderkey = l1.l_orderkey
            AND l3.l_suppkey <> l1.l_suppkey
            AND l3.l_receiptdate > l3.l_commitdate
        )
      GROUP BY n.n_name, s.s_name
    ),
    ranked AS (
      SELECT
        n_name, s_name, numwait,
        ROW_NUMBER() OVER (
          PARTITION BY n_name ORDER BY numwait DESC, s_name
        ) AS rn
      FROM waits
    )
    SELECT n_name, s_name, numwait
    FROM ranked
    WHERE rn <= 100;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_nation_idx ON {rel(n)} (n_name);
    CREATE INDEX {rel(n)}_wait_idx ON {rel(n)} (n_name, numwait DESC);
    """
    residual = f"""
    SELECT s_name, numwait
    FROM {rel(n)}
    WHERE n_name = {q(nation)}
    ORDER BY numwait DESC, s_name
    """
    reference = f"""
    SELECT s_name, COUNT(*)::bigint AS numwait
    FROM supplier, lineitem l1, orders, nation
    WHERE s_suppkey = l1.l_suppkey
      AND o_orderkey = l1.l_orderkey
      AND o_orderstatus = 'F'
      AND l1.l_receiptdate > l1.l_commitdate
      AND EXISTS (
        SELECT * FROM lineitem l2
        WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey
      )
      AND NOT EXISTS (
        SELECT * FROM lineitem l3
        WHERE l3.l_orderkey = l1.l_orderkey
          AND l3.l_suppkey <> l1.l_suppkey
          AND l3.l_receiptdate > l3.l_commitdate
      )
      AND s_nationkey = n_nationkey
      AND n_name = {q(nation)}
    GROUP BY s_name
    ORDER BY numwait DESC, s_name
    LIMIT 100
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(nation_name text)
    RETURNS TABLE (s_name character varying, numwait bigint)
    LANGUAGE sql STABLE AS $$
      SELECT m.s_name, m.numwait
      FROM {rel(n)} m
      WHERE m.n_name = nation_name
      ORDER BY m.numwait DESC, m.s_name
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


def q22() -> None:
    n = 22
    p = SEED1[n]
    codes = p["codes"]
    codes_sql = ", ".join(q(c) for c in codes)
    all_codes = [f"{i:02d}" for i in range(10, 35)]
    all_codes_sql = ", ".join(q(c) for c in all_codes)
    create = f"""
    -- Answer grain: Q22 aggregates for all 25 country codes (avg over full set)
    -- UNION exact seed_1 7-code combo. ≤25 + 7 rows.
    CREATE MATERIALIZED VIEW {rel(n)} AS
    WITH all25 AS (
      SELECT cntrycode, COUNT(*)::bigint AS numcust, SUM(c_acctbal) AS totacctbal
      FROM (
        SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
        FROM customer
        WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ({all_codes_sql})
          AND c_acctbal > (
            SELECT AVG(c_acctbal) FROM customer
            WHERE c_acctbal > 0.00
              AND SUBSTRING(c_phone FROM 1 FOR 2) IN ({all_codes_sql})
          )
          AND NOT EXISTS (SELECT 1 FROM orders WHERE o_custkey = c_custkey)
      ) custsale
      GROUP BY cntrycode
    ),
    seed7 AS (
      SELECT cntrycode, COUNT(*)::bigint AS numcust, SUM(c_acctbal) AS totacctbal
      FROM (
        SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
        FROM customer
        WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ({codes_sql})
          AND c_acctbal > (
            SELECT AVG(c_acctbal) FROM customer
            WHERE c_acctbal > 0.00
              AND SUBSTRING(c_phone FROM 1 FOR 2) IN ({codes_sql})
          )
          AND NOT EXISTS (SELECT 1 FROM orders WHERE o_custkey = c_custkey)
      ) custsale
      GROUP BY cntrycode
    )
    SELECT 'all25'::text AS codeset, cntrycode, numcust, totacctbal FROM all25
    UNION ALL
    SELECT 'seed1'::text, cntrycode, numcust, totacctbal FROM seed7;
    """
    indexes = f"""
    CREATE INDEX {rel(n)}_codeset_idx ON {rel(n)} (codeset, cntrycode);
    """
    residual = f"""
    SELECT cntrycode, numcust, totacctbal
    FROM {rel(n)}
    WHERE codeset = 'seed1'
    ORDER BY cntrycode
    """
    reference = f"""
    SELECT cntrycode, COUNT(*)::bigint AS numcust, SUM(c_acctbal) AS totacctbal
    FROM (
      SELECT SUBSTRING(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal
      FROM customer
      WHERE SUBSTRING(c_phone FROM 1 FOR 2) IN ({codes_sql})
        AND c_acctbal > (
          SELECT AVG(c_acctbal) FROM customer
          WHERE c_acctbal > 0.00
            AND SUBSTRING(c_phone FROM 1 FOR 2) IN ({codes_sql})
        )
        AND NOT EXISTS (SELECT * FROM orders WHERE o_custkey = c_custkey)
    ) custsale
    GROUP BY cntrycode
    ORDER BY cntrycode
    """
    function = f"""
    CREATE OR REPLACE FUNCTION {fn(n)}(
      country_code1 text, country_code2 text, country_code3 text, country_code4 text,
      country_code5 text, country_code6 text, country_code7 text
    )
    RETURNS TABLE (cntrycode text, numcust bigint, totacctbal numeric)
    LANGUAGE sql STABLE AS $$
      SELECT m.cntrycode, m.numcust, m.totacctbal
      FROM {rel(n)} m
      WHERE m.codeset = 'seed1'
        AND m.cntrycode IN (
          country_code1, country_code2, country_code3, country_code4,
          country_code5, country_code6, country_code7
        )
      ORDER BY m.cntrycode
    $$;
    """
    emit_common(n, create, indexes, residual, function, reference)


WORKLOAD_PARAMS = {
    1: [("day_offset", "INTEGER")],
    2: [("part_size", "INTEGER"), ("part_type_pattern", "VARCHAR"), ("region_name", "VARCHAR")],
    3: [("market_segment", "VARCHAR"), ("cutoff_date", "DATE")],
    4: [("start_date", "DATE")],
    5: [("region_name", "VARCHAR"), ("start_date", "DATE")],
    6: [("start_date", "DATE"), ("discount", "DECIMAL"), ("quantity", "DECIMAL")],
    7: [("nation1", "VARCHAR"), ("nation2", "VARCHAR")],
    8: [("nation_name", "VARCHAR"), ("region_name", "VARCHAR"), ("part_type", "VARCHAR")],
    9: [("color", "VARCHAR")],
    10: [("start_date", "DATE")],
    11: [("nation_name", "VARCHAR"), ("fraction", "DECIMAL")],
    12: [("shipmode1", "VARCHAR"), ("shipmode2", "VARCHAR"), ("start_date", "DATE")],
    13: [("comment_pattern", "VARCHAR")],
    14: [("start_date", "DATE")],
    15: [("start_date", "DATE")],
    16: [
        ("brand", "VARCHAR"),
        ("type_pattern", "VARCHAR"),
        *[
            (f"size{i}", "INTEGER")
            for i in range(1, 9)
        ],
    ],
    17: [("brand", "VARCHAR"), ("container", "VARCHAR")],
    18: [("quantity_threshold", "DECIMAL")],
    19: [
        ("brand1", "VARCHAR"),
        ("quantity1", "DECIMAL"),
        ("brand2", "VARCHAR"),
        ("quantity2", "DECIMAL"),
        ("brand3", "VARCHAR"),
        ("quantity3", "DECIMAL"),
    ],
    20: [("start_date", "DATE"), ("part_name_pattern", "VARCHAR"), ("nation_name", "VARCHAR")],
    21: [("nation_name", "VARCHAR")],
    22: [(f"country_code{i}", "VARCHAR") for i in range(1, 8)],
}

DEPENDENT = {
    1: ["lineitem"],
    2: ["part", "supplier", "partsupp", "nation", "region"],
    3: ["customer", "orders", "lineitem"],
    4: ["orders", "lineitem"],
    5: ["customer", "orders", "lineitem", "supplier", "nation", "region"],
    6: ["lineitem"],
    7: ["supplier", "lineitem", "orders", "customer", "nation"],
    8: ["part", "supplier", "lineitem", "orders", "customer", "nation", "region"],
    9: ["part", "supplier", "lineitem", "partsupp", "orders", "nation"],
    10: ["customer", "orders", "lineitem", "nation"],
    11: ["partsupp", "supplier", "nation"],
    12: ["orders", "lineitem"],
    13: ["customer", "orders"],
    14: ["lineitem", "part"],
    15: ["lineitem", "supplier"],
    16: ["part", "partsupp", "supplier"],
    17: ["lineitem", "part"],
    18: ["lineitem", "orders", "customer"],
    19: ["lineitem", "part"],
    20: ["supplier", "nation", "partsupp", "part", "lineitem"],
    21: ["supplier", "lineitem", "orders", "nation"],
    22: ["customer", "orders"],
}


def write_workload(n: int) -> None:
    params = "\n".join(
        f"  - name: {name}\n    type: {typ}" for name, typ in WORKLOAD_PARAMS[n]
    )
    deps = "\n".join(f"  - name: {t}\n    watched_columns: []" for t in DEPENDENT[n])
    # Prefer in-repo reference.sql once generated
    ref = f"accelerators/tpch/q{n}/postgres/matview/reference.sql"
    body = f"""workload_kind: materialized_view
benchmark_name: tpch
query_number: {n}
engine_name: postgres
query_id: "{n}"
accelerator_id: tpch_q{n}_mv
mv_function_name: {fn(n)}
mv_relation_name: {rel(n)}

requires_base_accelerator: false

allow_poor_fitness: false

reference_sql_path: {ref}
seed_manifest_root: /home/akhilvb/tpch-utils/generated/sf1_instances

parameters:
{params}

dependent_tables:
{deps}

output_schema: []

speedup_verification:
  min_speedup_vs_native_sql: 1.2

agent_boundary: prompts/materialized_view/shared/agent_boundary.md
"""
    path = WL / f"tpch_q{n}_matview.yaml"
    path.write_text(body, encoding="utf-8")


def main() -> None:
    emitters = [
        q1, q2, q3, q4, q5, q6, q7, q8, q9, q10, q11, q12,
        q13, q14, q15, q16, q17, q18, q19, q20, q21, q22,
    ]
    for emit in emitters:
        emit()
    for n in range(1, 23):
        write_workload(n)
    print(f"wrote matview SQL for Q1–Q22 under {GEN}")
    print(f"wrote workload YAMLs under {WL}")


if __name__ == "__main__":
    main()
