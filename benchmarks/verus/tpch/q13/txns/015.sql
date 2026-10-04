UPDATE orders SET o_custkey = (SELECT MAX(c_custkey) + 1 FROM customer)
WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_name = 'Q13 new 1');
UPDATE customer SET c_custkey = (SELECT MAX(c_custkey) + 1 FROM customer)
WHERE c_name = 'Q13 new 1';
INSERT INTO orders
SELECT (SELECT MAX(o_orderkey) + 1 FROM orders), c_custkey, 'O', 220.00,
       DATE '1995-03-22', '3-MEDIUM', 'Clerk#Q13', 0, 'Renumbered account order'
FROM customer WHERE c_name = 'Q13 new 1';
