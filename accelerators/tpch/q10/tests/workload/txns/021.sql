INSERT INTO lineitem
SELECT l_orderkey, l_partkey, l_suppkey, 1, 15.00, 160000.00, 0.05, l_tax, 'R',
       l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode, l_comment
FROM lineitem WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-11')
  AND l_linenumber = 2;
