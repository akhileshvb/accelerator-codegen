INSERT INTO orders
SELECT (SELECT max(o_orderkey) + 1 FROM orders),
       (SELECT c_custkey FROM customer WHERE c_comment = 'q22-reactivate'),
       'O', 500.00, DATE '1996-03-03', o_orderpriority,
       o_clerk, o_shippriority, 'q22-reactivation-order'
FROM orders ORDER BY o_orderkey LIMIT 1;
