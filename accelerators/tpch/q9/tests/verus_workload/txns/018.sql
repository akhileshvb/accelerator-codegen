UPDATE part SET p_name = 'forest green polished' WHERE p_partkey = (SELECT pkey FROM q9_test_keys);
