DELETE FROM lineitem WHERE rowid = (SELECT max(rowid) FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys) AND l_partkey = (SELECT pkey FROM q9_test_keys));
DELETE FROM partsupp WHERE rowid = (SELECT max(rowid) FROM partsupp WHERE ps_partkey = (SELECT pkey FROM q9_test_keys));
DELETE FROM nation WHERE rowid = (SELECT max(rowid) FROM nation WHERE n_nationkey = (SELECT nkey FROM q9_test_keys));
DELETE FROM supplier WHERE rowid = (SELECT max(rowid) FROM supplier WHERE s_suppkey = (SELECT skey FROM q9_test_keys));
DELETE FROM orders WHERE rowid = (SELECT max(rowid) FROM orders WHERE o_orderkey = (SELECT okey FROM q9_test_keys));
DELETE FROM part WHERE rowid = (SELECT max(rowid) FROM part WHERE p_partkey = (SELECT pkey FROM q9_test_keys));
