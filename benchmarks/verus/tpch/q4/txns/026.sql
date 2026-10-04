UPDATE lineitem SET l_receiptdate = l_commitdate
WHERE l_orderkey IN (
    SELECT o_orderkey FROM orders
    WHERE o_comment NOT LIKE 'q4:%' AND o_orderdate >= DATE '1994-10-01' AND o_orderdate < DATE '1995-01-01'
      AND EXISTS (SELECT 1 FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
    ORDER BY o_orderkey LIMIT 5
);
