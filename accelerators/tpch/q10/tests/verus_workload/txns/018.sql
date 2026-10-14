INSERT INTO nation
SELECT (SELECT max(n_nationkey) + 1 FROM nation), 'Q10 Coastal', n_regionkey, 'New sales territory'
FROM nation ORDER BY n_nationkey LIMIT 1;
INSERT INTO customer
SELECT (SELECT max(c_custkey) + 1 FROM customer), 'Q10-New-C', 'Coastal billing address',
       (SELECT n_nationkey FROM nation WHERE n_name = 'Q10 Coastal'), '20-300-300-3000',
       1500.00, 'AUTOMOBILE', 'New return customer';
INSERT INTO orders
SELECT (SELECT max(o_orderkey) + 1 FROM orders),
       (SELECT c_custkey FROM customer WHERE c_name = 'Q10-New-C'),
       'F', 350000.00, DATE '1994-10-01', '1-URGENT', o_clerk, 0, 'Q10-New-C'
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-New-C'), l_partkey, l_suppkey,
       new_line, 40.00, 175000.00, 0.03, 0.02, 'R', 'F', DATE '1994-10-15',
       DATE '1994-10-12', DATE '1994-10-20', l_shipinstruct, l_shipmode, 'Q10-New-C'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN range(1, 3) AS new_lines(new_line);
