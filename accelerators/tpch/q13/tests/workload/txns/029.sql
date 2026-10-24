CREATE TABLE q13_saved_orders AS SELECT * FROM orders;
CREATE TABLE q13_saved_customers AS SELECT * FROM customer;
UPDATE orders SET o_comment = 'unusual all excluded accounts';
