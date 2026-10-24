DELETE FROM part WHERE p_partkey BETWEEN
(SELECT part_base + 126 FROM q2_test_keys) AND (SELECT part_base + 128 FROM q2_test_keys);
DELETE FROM partsupp WHERE ps_partkey BETWEEN
(SELECT part_base + 126 FROM q2_test_keys) AND (SELECT part_base + 128 FROM q2_test_keys);
