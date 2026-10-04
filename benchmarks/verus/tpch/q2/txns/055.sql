UPDATE part SET p_type = 'POLISHED BRASS' WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND p_type = 'STANDARD STEEL';
UPDATE partsupp SET ps_supplycost = -101.00 WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND ps_suppkey = (SELECT supp_base + 3 FROM q2_test_keys);
