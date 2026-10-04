INSERT INTO customer
SELECT key_base.max_key + seed.ordinal, 'Q13 new ' || seed.ordinal,
       '24 Commerce Avenue', template.c_nationkey, '13-555-0200', 500.00,
       'AUTOMOBILE', 'New business account'
FROM (SELECT MAX(c_custkey) AS max_key FROM customer) key_base
CROSS JOIN range(1, 3) seed(ordinal)
CROSS JOIN (SELECT c_nationkey FROM customer ORDER BY c_custkey LIMIT 1) template;
INSERT INTO orders
SELECT key_base.max_key + ROW_NUMBER() OVER (ORDER BY customers.c_custkey, series.ordinal),
       customers.c_custkey, 'O', 700.00, DATE '1995-03-20', '2-HIGH',
       'Clerk#Q13', 0, 'New account opening order'
FROM customer customers CROSS JOIN range(1, 4) series(ordinal)
CROSS JOIN (SELECT MAX(o_orderkey) AS max_key FROM orders) key_base
WHERE customers.c_name IN ('Q13 new 1', 'Q13 new 2');
