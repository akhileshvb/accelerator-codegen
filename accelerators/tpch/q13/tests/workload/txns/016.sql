DELETE FROM orders WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 customer 5');
INSERT INTO orders
SELECT key_base.max_key + series.ordinal, customers.c_custkey, 'O', 90.00,
       DATE '1995-03-22', '5-LOW', 'Clerk#Q13', 0, 'Replacement order'
FROM customer customers CROSS JOIN range(1, 5) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base
WHERE customers.c_name = 'Q13 customer 5';
