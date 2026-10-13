SELECT
  100.00 * SUM(promo_rev) / SUM(all_rev) AS promo_revenue
FROM accel_tpch_q14_matview
WHERE l_shipdate >= DATE '1995-06-01'
  AND l_shipdate < DATE '1995-06-01' + INTERVAL '1' month
