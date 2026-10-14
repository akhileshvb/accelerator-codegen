UPDATE part SET p_size = 10 WHERE p_partkey IN (SELECT p_partkey FROM q2_test_parts ORDER BY p_partkey LIMIT 60);
UPDATE partsupp SET ps_supplycost = ps_supplycost + 1.00 WHERE ps_partkey IN (SELECT p_partkey FROM q2_test_parts WHERE p_partkey <> (SELECT part_base + 125 FROM q2_test_keys));
