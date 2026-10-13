-- Pre-counted distinct suppliers by brand/type/size (non-complaint only).
CREATE MATERIALIZED VIEW accel_tpch_q16_matview AS
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
