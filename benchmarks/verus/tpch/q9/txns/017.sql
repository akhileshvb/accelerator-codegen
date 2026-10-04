UPDATE lineitem SET l_extendedprice = l_extendedprice + 333.33, l_quantity = 9.75 WHERE l_partkey = (SELECT pkey FROM q9_test_keys);
UPDATE partsupp SET ps_supplycost = 47.31 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys);
UPDATE orders SET o_orderdate = DATE '1997-01-01' WHERE o_orderkey = (SELECT okey FROM q9_test_keys);
UPDATE lineitem SET l_extendedprice = l_extendedprice + 5.25 WHERE (l_orderkey, l_linenumber) IN (SELECT o_orderkey, l_linenumber FROM q9_test_seed);
