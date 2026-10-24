UPDATE nation SET n_name = (SELECT n_name FROM q9_test_nation) || ' ' WHERE n_nationkey = (SELECT nkey FROM q9_test_keys) + 10;
UPDATE supplier SET s_nationkey = (SELECT nkey FROM q9_test_keys) + 10 WHERE s_suppkey = (SELECT skey FROM q9_test_keys) + 10;
UPDATE partsupp SET ps_supplycost = 0.00 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys) + 10;
UPDATE lineitem SET l_extendedprice = 0.00, l_discount = 1.00, l_quantity = 0.00 WHERE l_partkey = (SELECT pkey FROM q9_test_keys) + 10;
