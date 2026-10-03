-- TPC-H Q3 (catalog text), :segment as $1, :date as $2, LIMIT 10.
SELECT l_orderkey, SUM(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority
FROM customer, orders, lineitem
WHERE c_mktsegment = $1
  AND c_custkey = o_custkey
  AND l_orderkey = o_orderkey
  AND o_orderdate < $2
  AND l_shipdate > $2
GROUP BY l_orderkey, o_orderdate, o_shippriority
ORDER BY revenue DESC, o_orderdate
LIMIT 10
