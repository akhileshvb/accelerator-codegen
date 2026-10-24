INSERT INTO orders
SELECT (SELECT MAX(o_orderkey) + 1 FROM orders), c_custkey, 'O', 480.00,
       DATE '1995-03-17', '3-MEDIUM', 'Clerk#Q13', 0, 'Repeat new order'
FROM customer WHERE c_name = 'Q13 customer 1';
