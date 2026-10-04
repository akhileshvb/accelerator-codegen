UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM q9_test_nation) WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
