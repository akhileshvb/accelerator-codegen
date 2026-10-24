UPDATE orders SET o_comment = 'Reconciliation completed'
WHERE o_comment = 'unusual reconciliation accounts';
INSERT INTO orders
SELECT (SELECT MAX(o_orderkey) + 1 FROM orders), c_custkey, 'O', 199.99,
       DATE '1995-03-24', '2-HIGH', 'Clerk#Q13', 0, 'Post reconciliation order'
FROM customer WHERE c_name = 'Q13 customer 4';
