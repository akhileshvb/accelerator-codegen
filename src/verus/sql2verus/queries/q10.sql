-- TPC-H Q10 (catalog text), :start_date as $1, $2 = $1 + 3 months (derived end date: sql2verus cannot add months to a non-literal date), LIMIT 20.
SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal, n_name, c_address, c_phone, c_comment
FROM customer JOIN orders ON c_custkey = o_custkey
  JOIN lineitem ON l_orderkey = o_orderkey JOIN nation ON c_nationkey = n_nationkey
WHERE o_orderdate >= $1
  AND o_orderdate < $2 AND l_returnflag = 'R'
GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
ORDER BY revenue DESC, c_custkey
LIMIT 20
