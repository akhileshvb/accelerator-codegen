UPDATE nation SET n_regionkey = (SELECT region_base FROM q2_test_keys) WHERE n_nationkey = (SELECT nation_base FROM q2_test_keys);
