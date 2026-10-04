UPDATE part SET p_type = 'BRASS ' WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND p_type = 'BRASS';
