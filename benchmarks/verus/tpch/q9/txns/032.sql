INSERT INTO orders SELECT q9_test_orders.* REPLACE (k.okey AS o_orderkey, DATE '1998-01-01' AS o_orderdate) FROM q9_test_orders CROSS JOIN q9_test_keys k;
INSERT INTO lineitem SELECT * FROM q9_test_removed_order_lines;
