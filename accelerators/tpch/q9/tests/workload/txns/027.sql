CREATE TABLE q9_test_removed_supplier_lines AS SELECT * FROM lineitem WHERE l_suppkey = (SELECT skey FROM q9_test_keys);
CREATE TABLE q9_test_removed_supplier_pairs AS SELECT * FROM partsupp WHERE ps_suppkey = (SELECT skey FROM q9_test_keys);
DELETE FROM lineitem WHERE l_suppkey = (SELECT skey FROM q9_test_keys);
DELETE FROM partsupp WHERE ps_suppkey = (SELECT skey FROM q9_test_keys);
DELETE FROM supplier WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
