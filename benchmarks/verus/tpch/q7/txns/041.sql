INSERT INTO nation SELECT c_nationkey, 'GERMANY', n_regionkey, 'q7 restored Germany' FROM customer CROSS JOIN (SELECT n_regionkey FROM nation ORDER BY n_nationkey LIMIT 1) AS source WHERE c_comment='q7 customer B';

INSERT INTO supplier
SELECT (SELECT l_suppkey FROM lineitem WHERE l_comment='q7 live 1'),
       s_name, s_address, (SELECT n_nationkey FROM nation WHERE n_name='FRANCE' ORDER BY n_nationkey LIMIT 1), s_phone, s_acctbal, 'q7 supplier A'
FROM supplier WHERE s_comment='q7 supplier B';

INSERT INTO customer
SELECT (SELECT o_custkey FROM orders WHERE o_comment='q7 live'),
       c_name, c_address, (SELECT n_nationkey FROM nation WHERE n_name='GERMANY' ORDER BY n_nationkey LIMIT 1), c_phone, c_acctbal, c_mktsegment, 'q7 customer A'
FROM customer WHERE c_comment='q7 customer B';
