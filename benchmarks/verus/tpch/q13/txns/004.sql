INSERT INTO orders
SELECT key_base.max_key + ROW_NUMBER() OVER (ORDER BY customers.c_custkey, series.ordinal),
       customers.c_custkey, 'O', 850.75, DATE '1995-03-19', '4-NOT SPECIFIED',
       'Clerk#Q13', 0, 'Monthly replenishment order'
FROM (SELECT c_custkey FROM customer WHERE c_name NOT LIKE 'Q13 %'
      ORDER BY c_custkey LIMIT 30) customers
CROSS JOIN range(1, 4) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base;
