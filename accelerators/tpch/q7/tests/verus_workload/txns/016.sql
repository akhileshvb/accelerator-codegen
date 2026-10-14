DELETE FROM orders WHERE o_comment='q7 C second';

DELETE FROM lineitem WHERE l_comment='q7 C second 1';

INSERT INTO orders
SELECT (SELECT max(o_orderkey) FROM orders)+batch.i+1, (SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1),
       'O', 3000.00, DATE '1995-02-01', o_orderpriority, o_clerk, o_shippriority,
       'q7 batch ' || CAST(batch.i AS VARCHAR)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source
CROSS JOIN range(12) AS batch(i);

INSERT INTO lineitem
SELECT o_orderkey, l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), lines.i+1, 10.00,
       100.01+lines.i*10.00, 0.04, 0.02, 'N', 'O',
       CASE WHEN lines.i=0 THEN DATE '1995-01-01'
            WHEN lines.i=1 THEN DATE '1996-12-31' ELSE DATE '1994-12-31' END,
       DATE '1995-01-02', DATE '1997-01-03', l_shipinstruct, l_shipmode,
       'q7 batch line ' || CAST(lines.i AS VARCHAR)
FROM orders CROSS JOIN (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source
CROSS JOIN range(3) AS lines(i)
WHERE o_comment LIKE 'q7 batch %';
