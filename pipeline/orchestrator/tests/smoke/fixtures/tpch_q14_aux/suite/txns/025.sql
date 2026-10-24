-- point_insert / lineitem, duplicating a join row: a second lineitem row with the very
-- (l_orderkey, l_linenumber) pair of an existing in-window line, on a PROMO partkey. The schema declares
-- no key, so this is valid and SQL counts both rows: the row must be summed twice, not deduplicated.
INSERT INTO lineitem
    (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
     l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode,
     l_comment)
SELECT l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
       l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode,
       'Q14DUPROW'
FROM lineitem
WHERE l_shipdate >= DATE '1995-06-01' AND l_shipdate < DATE '1995-07-01'
ORDER BY l_orderkey, l_linenumber LIMIT 1;
