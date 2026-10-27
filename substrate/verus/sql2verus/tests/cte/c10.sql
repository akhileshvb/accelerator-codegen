WITH k AS (SELECT o_cust FROM orders) SELECT c_id FROM cust WHERE EXISTS (SELECT 1 FROM k WHERE k.o_cust = c_id) AND c_id IN (SELECT o_cust FROM k WHERE o_cust > 3) ORDER BY c_id
