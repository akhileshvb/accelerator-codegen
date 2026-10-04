UPDATE region SET r_regionkey = -r_regionkey WHERE r_regionkey = (SELECT region_base FROM q2_test_keys);
UPDATE nation SET n_regionkey = -n_regionkey WHERE n_regionkey = (SELECT region_base FROM q2_test_keys);
UPDATE supplier SET s_suppkey = -s_suppkey WHERE s_suppkey = (SELECT supp_base FROM q2_test_keys);
UPDATE partsupp SET ps_suppkey = -ps_suppkey WHERE ps_suppkey = (SELECT supp_base FROM q2_test_keys);
