-- Day x discount x quantity revenue matrix (mirrors aux grain).
CREATE MATERIALIZED VIEW accel_tpch_q6_matview AS
SELECT
  l_shipdate,
  l_discount,
  l_quantity,
  SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
GROUP BY l_shipdate, l_discount, l_quantity;
