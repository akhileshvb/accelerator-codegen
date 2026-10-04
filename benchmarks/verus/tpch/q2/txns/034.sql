INSERT INTO part SELECT * FROM q2_test_parts WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
