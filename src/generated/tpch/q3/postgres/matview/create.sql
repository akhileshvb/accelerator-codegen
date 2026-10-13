-- Answer grain: Top-10 orders per (mktsegment × each day in March 1995).
-- 5 segments × 31 cutoffs × ≤10 ≈ 1550 rows.
CREATE MATERIALIZED VIEW accel_tpch_q3_matview AS
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
