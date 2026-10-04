UPDATE lineitem SET l_quantity = 17.25, l_extendedprice = CASE WHEN l_orderkey = (SELECT okey FROM q9_test_keys) THEN 1789.12 ELSE 2134.56 END, l_discount = 0.03 WHERE l_partkey = (SELECT pkey FROM q9_test_keys);
UPDATE partsupp SET ps_supplycost = 49.95 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys);
