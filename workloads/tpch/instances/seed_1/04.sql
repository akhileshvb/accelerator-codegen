SELECT
  o_orderpriority,
  COUNT(o_orderkey) AS order_count
FROM
  orders
WHERE
  o_orderdate >= DATE '1996-11-01'
  AND o_orderdate < DATE '1996-11-01' + interval '3' month
  AND EXISTS (
    SELECT
      *
    FROM
      lineitem
    WHERE
      l_orderkey = o_orderkey
      AND l_commitdate < l_receiptdate)
GROUP BY
  o_orderpriority
ORDER BY
  o_orderpriority;

