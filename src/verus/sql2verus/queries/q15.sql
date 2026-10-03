-- TPC-H Q15 (catalog text), :start_date as $1, $2 = $1 + 3 months (derived end date: sql2verus cannot add months to a non-literal date).
SELECT s_suppkey, s_name, s_address, s_phone, total_revenue
FROM supplier, (SELECT l_suppkey AS supplier_no, SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
                FROM lineitem WHERE l_shipdate >= $1
                  AND l_shipdate < $2
                GROUP BY l_suppkey) revenue0
WHERE s_suppkey = supplier_no
  AND total_revenue = (SELECT MAX(total_revenue)
                       FROM (SELECT l_suppkey AS supplier_no, SUM(l_extendedprice * (1 - l_discount)) AS total_revenue
                             FROM lineitem WHERE l_shipdate >= $1
                               AND l_shipdate < $2
                             GROUP BY l_suppkey) revenue1)
ORDER BY s_suppkey
