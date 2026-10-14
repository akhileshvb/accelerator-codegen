UPDATE part SET p_name = 'new green catalog' WHERE p_partkey IN ((SELECT pkey FROM q9_test_keys), (SELECT pkey FROM q9_test_keys) + 1);
UPDATE partsupp SET ps_supplycost = ps_supplycost + 1.23 WHERE ps_partkey IN ((SELECT pkey FROM q9_test_keys), (SELECT pkey FROM q9_test_keys) + 1);
