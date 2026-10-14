DELETE FROM nation WHERE n_nationkey = (SELECT nation_base FROM q2_test_keys);
