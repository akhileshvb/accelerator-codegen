INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 zero' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier Z' ORDER BY s_suppkey LIMIT 1), 2,
       10.00, 9999999999999.99, 0.00, 0.02, 'N', 'O', DATE '1995-01-01',
       DATE '1995-01-01' + INTERVAL 1 DAY, DATE '1995-01-01' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 zero 2'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO lineitem
SELECT (SELECT o_orderkey FROM orders WHERE o_comment='q7 zero' ORDER BY o_orderkey LIMIT 1), l_partkey, (SELECT s_suppkey FROM supplier WHERE s_comment='q7 supplier Z' ORDER BY s_suppkey LIMIT 1), 3,
       10.00, -9999999999999.99, 0.00, 0.02, 'N', 'O', DATE '1995-01-01',
       DATE '1995-01-01' + INTERVAL 1 DAY, DATE '1995-01-01' + INTERVAL 3 DAY,
       l_shipinstruct, l_shipmode, 'q7 zero 3'
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS source;

INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment='q7 zero 1';

UPDATE lineitem SET l_extendedprice=l_extendedprice+0.03 WHERE l_comment='q7 live 2';
