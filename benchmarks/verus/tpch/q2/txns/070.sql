DELETE FROM partsupp WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys)
AND ps_suppkey = (SELECT supp_base + 2 FROM q2_test_keys);
DELETE FROM supplier WHERE s_suppkey = (SELECT supp_base + 2 FROM q2_test_keys);
