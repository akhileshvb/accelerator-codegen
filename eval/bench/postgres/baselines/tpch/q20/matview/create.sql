-- Answer grain: qualifying suppliers per (color prefix × year × nation).
-- ~92 colors × 5 years × 25 nations × (few suppliers) — not partsupp×year.
CREATE MATERIALIZED VIEW accel_tpch_q20_matview AS
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
