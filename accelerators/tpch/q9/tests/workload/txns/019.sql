UPDATE part SET p_name = 'ocean green polished' WHERE p_partkey = (SELECT pkey FROM q9_test_keys) + 1;
