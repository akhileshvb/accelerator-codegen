INSERT INTO customer
SELECT (SELECT max(c_custkey)+1 FROM customer), c_name, c_address,
       (SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1), c_phone, c_acctbal, c_mktsegment, 'q7 customer C'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source;

INSERT INTO supplier
SELECT (SELECT max(s_suppkey)+1 FROM supplier), s_name, s_address,
       (SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1), s_phone, s_acctbal, 'q7 supplier C'
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) AS source;

INSERT INTO orders
SELECT (SELECT max(o_orderkey)+1 FROM orders), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer C' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 C first'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 C first' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier C' ORDER BY s_suppkey LIMIT 1), 1,
       10.00, 789.12, 0.05, 0.02, 'N', 'O', DATE '1995-06-15',
       DATE '1995-06-15' + INTERVAL 1 DAY, DATE '1995-06-15' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 C first 1'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO orders
SELECT (SELECT max(o_orderkey)+1 FROM orders), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer C' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 C second'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 C second' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier C' ORDER BY s_suppkey LIMIT 1), 1,
       10.00, 345.67, 0.05, 0.02, 'N', 'O', DATE '1996-08-10',
       DATE '1996-08-10' + INTERVAL 1 DAY, DATE '1996-08-10' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 C second 1'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;
