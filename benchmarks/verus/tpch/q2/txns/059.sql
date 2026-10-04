UPDATE region SET r_name = 'EUROPE ' WHERE r_regionkey = -(SELECT region_base FROM q2_test_keys);
UPDATE nation SET n_name = ' germany ' WHERE n_nationkey = -(SELECT nation_base FROM q2_test_keys);
UPDATE supplier SET s_name = 'é supplier', s_address = '', s_phone = '', s_comment = '' WHERE s_suppkey = -(SELECT supp_base FROM q2_test_keys);
