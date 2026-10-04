UPDATE orders SET o_orderdate = DATE '2001-01-01' WHERE o_orderkey IN ((SELECT okey FROM q9_test_keys) + 1, (SELECT okey FROM q9_test_keys) + 2, (SELECT okey FROM q9_test_keys) + 3, (SELECT okey FROM q9_test_keys) + 100);
INSERT INTO orders SELECT q9_test_orders.* REPLACE (k.okey + 10 AS o_orderkey, DATE '2000-12-31' AS o_orderdate) FROM q9_test_orders CROSS JOIN q9_test_keys k;
UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.01 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys) + 10;
UPDATE part SET p_name = ' green%_ evergreen ' WHERE p_partkey = (SELECT pkey FROM q9_test_keys) + 10;
