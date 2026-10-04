INSERT INTO orders
SELECT (SELECT l_orderkey FROM lineitem WHERE l_comment='q7 orphan'), (SELECT c_custkey FROM customer WHERE c_comment='q7 customer A' ORDER BY c_custkey LIMIT 1), 'O', 1500.00, DATE '1995-01-02',
       o_orderpriority, o_clerk, o_shippriority, 'q7 orphan'
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source;
