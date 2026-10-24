INSERT INTO orders
SELECT key_base.max_key + ROW_NUMBER() OVER (ORDER BY customers.c_custkey, series.ordinal),
       customers.c_custkey, 'O', 200.00, DATE '1995-03-18', '3-MEDIUM',
       'Clerk#Q13', 0, 'Small batch order'
FROM customer customers CROSS JOIN range(1, 3) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base
WHERE customers.c_name IN ('Q13 customer 2', 'Q13 customer 3', 'Q13 customer 4');
