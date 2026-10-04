UPDATE part SET p_name = upper(p_name) WHERE p_partkey = (SELECT pkey FROM q9_test_keys) + 10;
DELETE FROM orders WHERE o_orderkey = (SELECT okey FROM q9_test_keys) + 10;
