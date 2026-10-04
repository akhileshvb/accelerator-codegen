INSERT INTO orders SELECT * FROM orders WHERE o_comment = 'q8-order-01';
INSERT INTO customer SELECT * FROM customer WHERE c_comment = 'q8-customer-a';
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'q8-line-01-1';
