UPDATE partsupp SET ps_supplycost = 30.00 WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND ps_suppkey = (SELECT supp_base + 1 FROM q2_test_keys);
