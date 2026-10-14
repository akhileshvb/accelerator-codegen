INSERT INTO part SELECT * FROM part WHERE p_partkey = (SELECT pkey FROM q9_test_keys);
INSERT INTO orders SELECT * FROM orders WHERE o_orderkey = (SELECT okey FROM q9_test_keys);
INSERT INTO supplier SELECT * FROM supplier WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
INSERT INTO nation SELECT * FROM nation WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
INSERT INTO partsupp SELECT * FROM partsupp WHERE ps_partkey = (SELECT pkey FROM q9_test_keys);
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys) AND l_partkey = (SELECT pkey FROM q9_test_keys);
