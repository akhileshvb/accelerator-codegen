INSERT INTO supplier
SELECT (SELECT max(s_suppkey)+1 FROM supplier), s_name, s_address,
       (SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1), s_phone, s_acctbal, 'q7 supplier A'
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) AS source;

INSERT INTO supplier
SELECT (SELECT max(s_suppkey)+1 FROM supplier), s_name, s_address,
       (SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1), s_phone, s_acctbal, 'q7 supplier B'
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) AS source;

INSERT INTO customer
SELECT (SELECT max(c_custkey)+1 FROM customer), c_name, c_address,
       (SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1), c_phone, c_acctbal, c_mktsegment, 'q7 customer A'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source;

INSERT INTO customer
SELECT (SELECT max(c_custkey)+1 FROM customer), c_name, c_address,
       (SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1), c_phone, c_acctbal, c_mktsegment, 'q7 customer B'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source;

INSERT INTO orders
SELECT (SELECT max(o_orderkey)+1 FROM orders), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 point'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 point' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 1,
       10.00, 1000.00, 0.05, 0.02, 'N', 'O', DATE '1995-03-15',
       DATE '1995-03-15' + INTERVAL 1 DAY, DATE '1995-03-15' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 point 1'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 point' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 2,
       10.00, 2500.25, 0.07, 0.02, 'N', 'O', DATE '1996-07-20',
       DATE '1996-07-20' + INTERVAL 1 DAY, DATE '1996-07-20' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 point 2'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 point' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 3,
       10.00, 300.00, 0.05, 0.02, 'N', 'O', DATE '1994-12-31',
       DATE '1994-12-31' + INTERVAL 1 DAY, DATE '1994-12-31' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 point 3'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO orders
SELECT (SELECT max(o_orderkey)+1 FROM orders), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 window'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 window' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 1,
       10.00, 111.11, 0.05, 0.02, 'N', 'O', DATE '1994-12-31',
       DATE '1994-12-31' + INTERVAL 1 DAY, DATE '1994-12-31' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 window 1'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 window' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier A' ORDER BY s_suppkey LIMIT 1), 2,
       10.00, 222.22, 0.05, 0.02, 'N', 'O', DATE '1997-01-01',
       DATE '1997-01-01' + INTERVAL 1 DAY, DATE '1997-01-01' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 window 2'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;
