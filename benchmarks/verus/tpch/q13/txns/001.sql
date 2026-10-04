INSERT INTO orders
SELECT (SELECT MAX(o_orderkey) + 1 FROM orders), c_custkey, 'O', 315.25,
       DATE '1995-03-16', '2-HIGH', 'Clerk#Q13', 0, 'First new order'
FROM customer WHERE c_name = 'Q13 customer 1';
