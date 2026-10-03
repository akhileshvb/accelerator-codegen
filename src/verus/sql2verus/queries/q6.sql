-- TPC-H Q6 (catalog text), :start_date as $1, :discount as $2 (BETWEEN $2 - 0.01 AND $2 + 0.01), :quantity as $3, $4 = $1 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT SUM(l_extendedprice * l_discount) AS revenue
FROM lineitem
WHERE l_shipdate >= $1 AND l_shipdate < $4
  AND l_discount BETWEEN $2 - 0.01 AND $2 + 0.01 AND l_quantity < $3
