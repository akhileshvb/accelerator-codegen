-- Answer-level: sum(extendedprice) for qty < 0.2*avg(qty), by brand×container.
CREATE MATERIALIZED VIEW accel_tpch_q17_matview AS
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
