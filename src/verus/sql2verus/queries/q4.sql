-- TPC-H Q4 (catalog text), :start_date as $1, $2 = $1 + 3 months (derived end date: sql2verus cannot add months to a non-literal date).
SELECT o_orderpriority, COUNT(o_orderkey) AS order_count
FROM orders
WHERE o_orderdate >= $1
  AND o_orderdate < $2
  AND EXISTS (SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
GROUP BY o_orderpriority
ORDER BY o_orderpriority
