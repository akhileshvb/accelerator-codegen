INSERT INTO orders
SELECT (SELECT max(o_orderkey) + 1 FROM orders),
       (SELECT c_custkey FROM customer JOIN orders ON c_custkey = o_custkey
        JOIN lineitem ON o_orderkey = l_orderkey
        WHERE c_name NOT LIKE 'Q10-%' AND o_orderdate >= DATE '1993-10-01'
          AND o_orderdate < DATE '1994-01-01' AND l_returnflag = 'R'
        GROUP BY c_custkey ORDER BY sum(l_extendedprice * (1 - l_discount)) DESC, c_custkey LIMIT 1),
       'F', 180000.00, DATE '1993-11-15', '2-HIGH', o_clerk, 0, 'Q10-New-A'
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment = 'Q10-New-A'),
       l_partkey, l_suppkey, new_line, 20.00, 90000.00, 0.04, 0.02, 'R', 'F',
       DATE '1993-11-20', DATE '1993-11-18', DATE '1993-11-25', l_shipinstruct, l_shipmode, 'Q10-New-A'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN range(1, 3) AS new_lines(new_line);
