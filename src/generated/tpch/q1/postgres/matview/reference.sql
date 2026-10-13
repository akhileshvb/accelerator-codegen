SELECT
  l_returnflag,
  l_linestatus,
  SUM(l_quantity) AS sum_qty,
  SUM(l_extendedprice) AS sum_base_price,
  SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
  SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
  SUM(l_quantity) / COUNT(*) AS avg_qty,
  SUM(l_extendedprice) / COUNT(*) AS avg_price,
  SUM(l_discount) / COUNT(*) AS avg_disc,
  COUNT(*)::bigint AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '103' DAY
GROUP BY l_returnflag, l_linestatus
