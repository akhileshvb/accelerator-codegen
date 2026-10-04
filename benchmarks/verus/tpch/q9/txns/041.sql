UPDATE lineitem SET l_orderkey = (SELECT okey FROM q9_test_keys) + 100 WHERE l_orderkey = (SELECT okey FROM q9_test_keys) + 1 AND l_linenumber = 1;
