UPDATE part SET p_name = 'forest blue polished' WHERE p_partkey = (SELECT pkey FROM q9_test_keys);
