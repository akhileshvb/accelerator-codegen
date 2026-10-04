UPDATE partsupp SET ps_supplycost = -0.01 WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND ps_suppkey = (SELECT supp_base FROM q2_test_keys);
UPDATE part SET p_type = 'BRASS', p_mfgr = ' Brass Works ' WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
UPDATE region SET r_name = 'EUROPE' WHERE r_regionkey = (SELECT region_base FROM q2_test_keys);
