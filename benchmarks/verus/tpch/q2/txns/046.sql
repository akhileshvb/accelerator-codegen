UPDATE partsupp SET ps_partkey = -ps_partkey WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys);
UPDATE part SET p_partkey = -p_partkey, p_size = 2147483647, p_mfgr = 'É Brass Works' WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
