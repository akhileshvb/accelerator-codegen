CREATE TABLE q9_test_removed_pair_lines AS SELECT * FROM lineitem WHERE l_partkey = (SELECT pkey FROM q9_test_keys);
DELETE FROM lineitem WHERE l_partkey = (SELECT pkey FROM q9_test_keys);
DELETE FROM partsupp WHERE ps_partkey = (SELECT pkey FROM q9_test_keys);
