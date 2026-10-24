UPDATE part SET p_size = 10 WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
