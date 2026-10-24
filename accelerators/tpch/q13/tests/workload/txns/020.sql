INSERT INTO orders
SELECT key_base.max_key + ROW_NUMBER() OVER (ORDER BY customers.c_custkey, series.ordinal),
       customers.c_custkey, 'O', 145.00, DATE '1995-03-23', '3-MEDIUM',
       'Clerk#Q13', 0, 'Pending ' || RIGHT(customers.c_name, 1) || ' order'
FROM customer customers CROSS JOIN range(1, 3) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base
WHERE customers.c_name LIKE 'Q13 pending %';
