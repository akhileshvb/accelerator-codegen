INSERT INTO customer SELECT * FROM customer WHERE c_name = 'Q13 customer 4';
INSERT INTO customer SELECT * FROM customer WHERE c_name = 'Q13 customer 4';
INSERT INTO orders
SELECT * FROM orders
WHERE o_custkey = (SELECT MIN(c_custkey) FROM customer WHERE c_name = 'Q13 customer 4')
  AND o_comment NOT LIKE '%unusual%accounts%'
ORDER BY o_orderkey LIMIT 1;
INSERT INTO customer
SELECT * FROM customer WHERE c_name = 'Q13 customer 8';
