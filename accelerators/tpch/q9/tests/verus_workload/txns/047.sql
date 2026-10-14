UPDATE nation SET n_name = lower(n_name) WHERE n_nationkey = (SELECT nkey FROM q9_test_keys) + 10;
UPDATE lineitem SET l_extendedprice = 9999999999999.99, l_discount = 0.01, l_quantity = 0.01 WHERE l_partkey = (SELECT pkey FROM q9_test_keys) + 10 AND l_linenumber % 2 = 0;
UPDATE lineitem SET l_extendedprice = 0.01, l_discount = 0.00, l_quantity = 9999999999999.99 WHERE l_partkey = (SELECT pkey FROM q9_test_keys) + 10 AND l_linenumber % 2 = 1;
UPDATE partsupp SET ps_supplycost = 0.01 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys) + 10;
