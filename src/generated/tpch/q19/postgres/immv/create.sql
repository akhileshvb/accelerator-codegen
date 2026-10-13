-- Incrementally maintainable matview (pg_ivm) for TPC-H Q19.
-- Same residual grain as matview where possible.
SELECT pgivm.create_immv(
  'accel_tpch_q19_immv',
  $immv$
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
  p.p_size
$immv$
);
