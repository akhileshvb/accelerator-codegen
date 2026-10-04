DELETE FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys) + 2;
DELETE FROM orders WHERE o_orderkey = (SELECT okey FROM q9_test_keys) + 2;
