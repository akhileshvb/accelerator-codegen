SELECT
  l_returnflag,
  l_linestatus,
  SUM(sum_qty) AS sum_qty,
  SUM(sum_base_price) AS sum_base_price,
  SUM(sum_disc_price) AS sum_disc_price,
  SUM(sum_charge) AS sum_charge,
  SUM(sum_qty) / SUM(count_order) AS avg_qty,
  SUM(sum_base_price) / SUM(count_order) AS avg_price,
  SUM(sum_discount) / SUM(count_order) AS avg_disc,
  SUM(count_order) AS count_order
FROM accel_tpch_q1_matview
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '103' DAY
GROUP BY l_returnflag, l_linestatus
