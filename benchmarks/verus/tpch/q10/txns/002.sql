INSERT INTO lineitem
SELECT (SELECT max(o_orderkey) + 1 FROM orders), l_partkey, l_suppkey, 1, 30.00,
       140000.00, 0.02, 0.02, 'R', 'F', DATE '1993-10-15', DATE '1993-10-12',
       DATE '1993-10-20', l_shipinstruct, l_shipmode, 'Q10-New-B'
FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;
INSERT INTO orders
SELECT (SELECT l_orderkey FROM lineitem WHERE l_comment = 'Q10-New-B'),
       (SELECT c_custkey FROM customer WHERE c_name = 'Q10-01'),
       'F', 140000.00, DATE '1993-10-01', '3-MEDIUM', o_clerk, 0, 'Q10-New-B'
FROM orders ORDER BY o_orderkey LIMIT 1;
