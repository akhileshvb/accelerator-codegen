UPDATE part SET p_name = 'ocean blue polished' WHERE p_partkey = (SELECT pkey FROM q9_test_keys) + 1;
