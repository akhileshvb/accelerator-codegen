INSERT INTO region SELECT * FROM region WHERE r_regionkey = (SELECT region_base FROM q2_test_keys);
INSERT INTO nation SELECT * FROM nation WHERE n_nationkey = (SELECT nation_base FROM q2_test_keys);
INSERT INTO supplier SELECT * FROM supplier WHERE s_suppkey = (SELECT supp_base FROM q2_test_keys);
INSERT INTO part SELECT * FROM part WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
INSERT INTO partsupp SELECT * FROM partsupp WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND ps_suppkey = (SELECT supp_base FROM q2_test_keys);
