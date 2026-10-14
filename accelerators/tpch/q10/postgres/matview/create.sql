-- Answer grain: Top-20 returned-revenue customers per first-of-month start
-- in [1993-02-01 .. 1995-01-01] (24 months × 20 ≈ 480 rows).
CREATE MATERIALIZED VIEW accel_tpch_q10_matview AS
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
