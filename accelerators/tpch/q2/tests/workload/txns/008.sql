UPDATE part SET p_mfgr = 'Manufacturer#2' WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
