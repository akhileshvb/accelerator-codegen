INSERT INTO customer
SELECT (SELECT MAX(c_custkey) + 1 FROM customer), 'Q13 new 1',
       c_address, c_nationkey, c_phone, 600.00, c_mktsegment, 'Reopened account'
FROM customer WHERE c_name = 'Q13 new 2';
INSERT INTO orders
SELECT key_base.max_key + series.ordinal, customers.c_custkey, 'O', 325.00,
       DATE '1995-03-21', '3-MEDIUM', 'Clerk#Q13', 0, 'Reopened account order'
FROM customer customers CROSS JOIN range(1, 3) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base
WHERE customers.c_name = 'Q13 new 1';
