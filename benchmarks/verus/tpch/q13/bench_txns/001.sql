INSERT INTO orders
SELECT (SELECT MAX(o_orderkey) + 1 FROM orders), c_custkey, 'O', 250.00,
       DATE '1995-03-27', '3-MEDIUM', 'Clerk#Q13', 0, 'Q13 benchmark point order'
FROM customer WHERE c_name = 'Q13 customer 1';
