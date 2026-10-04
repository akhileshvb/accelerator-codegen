DELETE FROM partsupp WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND ps_suppkey = (SELECT supp_base FROM q2_test_keys);
