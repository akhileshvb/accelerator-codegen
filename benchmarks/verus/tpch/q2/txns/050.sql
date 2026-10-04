DELETE FROM partsupp WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys);
