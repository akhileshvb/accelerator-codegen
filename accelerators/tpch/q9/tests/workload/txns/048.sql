DELETE FROM part WHERE p_partkey IN ((SELECT pkey FROM q9_test_keys), (SELECT pkey FROM q9_test_keys) + 1, (SELECT pkey FROM q9_test_keys) + 10);
DELETE FROM nation WHERE n_nationkey IN ((SELECT nkey FROM q9_test_keys), (SELECT nkey FROM q9_test_keys) + 10);
DELETE FROM orders WHERE o_orderkey IN ((SELECT okey FROM q9_test_keys), (SELECT okey FROM q9_test_keys) + 1, (SELECT okey FROM q9_test_keys) + 2, (SELECT okey FROM q9_test_keys) + 3, (SELECT okey FROM q9_test_keys) + 10, (SELECT okey FROM q9_test_keys) + 100);
DELETE FROM partsupp WHERE ps_partkey IN ((SELECT pkey FROM q9_test_keys), (SELECT pkey FROM q9_test_keys) + 1, (SELECT pkey FROM q9_test_keys) + 10);
DELETE FROM supplier WHERE s_suppkey IN ((SELECT skey FROM q9_test_keys), (SELECT skey FROM q9_test_keys) + 10);
DELETE FROM lineitem WHERE l_suppkey IN ((SELECT skey FROM q9_test_keys), (SELECT skey FROM q9_test_keys) + 10);
