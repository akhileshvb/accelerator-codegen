-- TPC-H Q14 (catalog text), :start_date as $1, $2 = $1 + 1 month (derived end date: sql2verus cannot add months to a non-literal date).
SELECT 100.00 * SUM(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)
       / SUM(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= $1 AND l_shipdate < $2
