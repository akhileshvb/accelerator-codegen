DELETE FROM lineitem WHERE l_suppkey = (SELECT skey FROM q9_test_keys);
DELETE FROM partsupp WHERE ps_suppkey = (SELECT skey FROM q9_test_keys);
DELETE FROM supplier WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
DELETE FROM nation WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
