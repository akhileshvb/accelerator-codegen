DELETE FROM region WHERE r_regionkey = (SELECT region_base FROM q2_test_keys);
