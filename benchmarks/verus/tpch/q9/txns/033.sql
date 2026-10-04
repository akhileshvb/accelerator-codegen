UPDATE lineitem SET l_extendedprice = l_extendedprice + 11.11, l_discount = 0.02 WHERE l_partkey = (SELECT pkey FROM q9_test_keys);
UPDATE orders SET o_orderdate = DATE '1998-12-31' WHERE o_orderkey = (SELECT okey FROM q9_test_keys) + 1;
UPDATE partsupp SET ps_supplycost = ps_supplycost - 0.25 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys);
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM q9_test_nation) WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
UPDATE nation SET n_name = 'Q9 MIXED REGION' WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
UPDATE part SET p_name = 'green revised catalog' WHERE p_partkey = (SELECT pkey FROM q9_test_keys);
