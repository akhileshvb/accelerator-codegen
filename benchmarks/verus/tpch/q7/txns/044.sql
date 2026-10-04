INSERT INTO nation SELECT (SELECT max(n_nationkey)+1 FROM nation), 'EDGE ZERO', n_regionkey, 'q7 zero nation' FROM nation ORDER BY n_nationkey LIMIT 1;

INSERT INTO supplier
SELECT (SELECT max(s_suppkey)+1 FROM supplier), s_name, s_address,
       (SELECT n_nationkey FROM nation WHERE n_name='EDGE ZERO' ORDER BY n_nationkey LIMIT 1), s_phone, s_acctbal, 'q7 supplier Z'
FROM (SELECT * FROM supplier ORDER BY s_suppkey LIMIT 1) AS source;

INSERT INTO customer
SELECT (SELECT max(c_custkey)+1 FROM customer), c_name, c_address,
       (SELECT n_nationkey FROM nation WHERE n_name='EDGE ZERO' ORDER BY n_nationkey LIMIT 1), c_phone, c_acctbal, c_mktsegment, 'q7 customer Z'
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS source;

INSERT INTO orders
SELECT (SELECT max(o_orderkey)+1 FROM orders), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer Z' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 zero'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 zero' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier Z' ORDER BY s_suppkey LIMIT 1), 1,
       10.00, 0.00, 0.00, 0.02, 'N', 'O', DATE '1995-01-01',
       DATE '1995-01-01' + INTERVAL 1 DAY, DATE '1995-01-01' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 zero 1'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO nation
SELECT n_nationkey, alias.name, n_regionkey, 'q7 text alias'
FROM nation CROSS JOIN (VALUES ('france'),('FRANCE '),(' FRANCE'),('日本'),('O''Brien'),('')) AS alias(name)
WHERE n_name='FRANCE';
