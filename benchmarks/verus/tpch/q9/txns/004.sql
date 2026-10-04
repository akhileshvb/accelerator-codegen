UPDATE partsupp SET ps_supplycost = 52.75 WHERE ps_partkey = (SELECT pkey FROM q9_test_keys) AND ps_suppkey = (SELECT skey FROM q9_test_keys);
