INSERT INTO orders SELECT q9_test_orders.* REPLACE (k.okey + 100 AS o_orderkey, DATE '1991-12-31' AS o_orderdate) FROM q9_test_orders CROSS JOIN q9_test_keys k;
