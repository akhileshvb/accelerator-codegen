-- Answer grain: top supplier(s) by revenue per month-start in
-- [1993-01-01 .. 1997-10-01] (58 starts × ≤few ties).
CREATE MATERIALIZED VIEW accel_tpch_q15_matview AS
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
