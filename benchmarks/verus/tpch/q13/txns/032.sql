INSERT INTO orders SELECT * FROM q13_saved_orders;
INSERT INTO customer SELECT * FROM q13_saved_customers;
DROP TABLE q13_saved_orders;
DROP TABLE q13_saved_customers;
