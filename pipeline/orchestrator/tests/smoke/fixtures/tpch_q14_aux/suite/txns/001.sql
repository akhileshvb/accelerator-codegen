-- point_insert / lineitem: a scratch row inside the query window, joined to a PROMO part, so it adds
-- to the numerator and the denominator. The bookmark l_comment holds the row for the later point
-- update (003), the key move (006), its undo (007) and the delete (008) (the query reads no comment
-- column).
INSERT INTO lineitem
    (l_orderkey, l_partkey, l_suppkey, l_linenumber, l_quantity, l_extendedprice, l_discount, l_tax,
     l_returnflag, l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipinstruct, l_shipmode,
     l_comment)
SELECT (SELECT COALESCE(MAX(l_orderkey), 0) FROM lineitem) + 1001,
       (SELECT p_partkey FROM part WHERE p_type LIKE 'PROMO%' ORDER BY p_partkey LIMIT 1),
       (SELECT MIN(s_suppkey) FROM supplier),
       1, 10.00, 4000.00, 0.05, 0.02, 'N', 'O',
       DATE '1995-06-12', DATE '1995-06-15', DATE '1995-06-20', 'NONE', 'AIR', 'Q14S1';
