-- Answer grain: (color token × nation × year) profit. LATERAL unnest of p_name.
-- ~92 colors × 25 nations × ~7 years ≈ 16k rows (not partkey).
CREATE MATERIALIZED VIEW accel_tpch_q9_matview AS
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
