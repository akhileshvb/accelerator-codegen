INSERT INTO orders
SELECT (SELECT max(o_orderkey) + 1 FROM orders),
       (SELECT c_custkey FROM customer WHERE c_comment = 'q22-onboarding'),
       'O', 2400.00, DATE '1996-02-15', o_orderpriority,
       o_clerk, o_shippriority, 'q22-second-order'
FROM orders ORDER BY o_orderkey LIMIT 1;
