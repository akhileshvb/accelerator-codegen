CREATE TABLE q9_test_removed_order_lines AS SELECT * FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys);
DELETE FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys);
DELETE FROM orders WHERE o_orderkey = (SELECT okey FROM q9_test_keys);
