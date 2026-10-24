DELETE FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys) + 1 AND l_linenumber IN (20, 21);
