-- Daily promo vs all revenue from lineitem⋈part.
CREATE MATERIALIZED VIEW accel_tpch_q14_matview AS
SELECT
  l.l_shipdate,
  SUM(CASE WHEN p.p_type LIKE 'PROMO%' THEN l.l_extendedprice * (1 - l.l_discount) ELSE 0 END) AS promo_rev,
  SUM(l.l_extendedprice * (1 - l.l_discount)) AS all_rev
FROM lineitem l
JOIN part p ON l.l_partkey = p.p_partkey
GROUP BY l.l_shipdate;
