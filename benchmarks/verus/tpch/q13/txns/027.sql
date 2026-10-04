UPDATE orders SET o_custkey = CAST('-2147483648' AS INTEGER), o_orderkey = 0
WHERE o_custkey = (SELECT MIN(c_custkey) FROM customer WHERE c_name = 'Q13 customer 8');
UPDATE customer SET c_custkey = CAST('-2147483648' AS INTEGER)
WHERE c_name = 'Q13 customer 8';
INSERT INTO orders
SELECT CAST('2147483647' AS INTEGER), CAST('2147483647' AS INTEGER),
       'O', 9999999999999.99, DATE '1995-03-25', '1-URGENT', 'Clerk#Q13',
       CAST('-2147483648' AS INTEGER), 'Boundary orphan order';
INSERT INTO customer
SELECT CAST('2147483647' AS INTEGER), 'Q13 boundary', c_address,
       c_nationkey, c_phone, -9999999999999.99, c_mktsegment, 'Adopt orphan order'
FROM customer WHERE c_name = 'Q13 customer 1';
DELETE FROM customer WHERE c_name = 'Q13 customer 3';
