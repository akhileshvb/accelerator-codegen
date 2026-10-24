-- bulk_insert / lineitem, RF1-shaped chunk: an 8-row chunk (one order's lineitems, new big orderkeys)
-- inside the window on the >2^32 part key of 016, with one extreme money value. Every row must reach the
-- state exactly: keys beyond 2^32 and a value at DECIMAL(15,2)'s limit are where 32-bit keys and
-- 64-bit sums break. l_shipmode is the bookmark 018 deletes the chunk by (the query reads it not).
INSERT INTO lineitem
    (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
     l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode,
     l_comment)
SELECT 4294967411 + g, 4294967311, (SELECT MIN(s_suppkey) FROM supplier), g, 10.00,
       CASE WHEN g = 1 THEN 9999999999999.99 ELSE 100.00 * g END, 0.02, 0.01, 'N', 'O',
       DATE '1995-06-20', DATE '1995-06-22', DATE '1995-06-25', 'NONE', 'Q14BIG', 'Q14BIG'
FROM range(1, 9) t(g);
