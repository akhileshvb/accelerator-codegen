-- TPC-H Q12 (catalog text), :shipmode1 as $1, :shipmode2 as $2, :start_date as $3, $4 = $3 + 1 year (derived end date: sql2verus cannot add months to a non-literal date).
SELECT l_shipmode,
       SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END) AS high_line_count,
       SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END) AS low_line_count
FROM orders, lineitem
WHERE o_orderkey = l_orderkey AND l_shipmode IN ($1, $2)
  AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate
  AND l_receiptdate >= $3
  AND l_receiptdate < $4
GROUP BY l_shipmode
ORDER BY l_shipmode
