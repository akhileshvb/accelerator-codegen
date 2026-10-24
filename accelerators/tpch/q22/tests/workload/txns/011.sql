INSERT INTO orders
SELECT (SELECT max(o_orderkey) FROM orders)
         + row_number() OVER (ORDER BY accounts.c_custkey),
       accounts.c_custkey, 'O', 1500.00, DATE '1996-03-01',
       source.o_orderpriority, source.o_clerk, source.o_shippriority, 'q22-batch-order'
FROM customer AS accounts
CROSS JOIN (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS source
WHERE accounts.c_comment = 'q22-batch';
