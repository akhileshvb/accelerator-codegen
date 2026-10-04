INSERT INTO orders
SELECT (SELECT max(o_orderkey)+1 FROM orders), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 live'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 live' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 1,
       10.00, 987.65, 0.01, 0.02, 'N', 'O', DATE '1995-01-01',
       DATE '1995-01-01' + INTERVAL 1 DAY, DATE '1995-01-01' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 live 1'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 live' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 2,
       10.00, 543.21, 0.08, 0.02, 'N', 'O', DATE '1996-12-31',
       DATE '1996-12-31' + INTERVAL 1 DAY, DATE '1996-12-31' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 live 2'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;
