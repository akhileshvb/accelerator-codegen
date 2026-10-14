UPDATE part SET p_partkey = p_partkey + 10 WHERE p_partkey = (SELECT pkey FROM q9_test_keys);
UPDATE partsupp SET ps_partkey = ps_partkey + 10, ps_suppkey = ps_suppkey + 10 WHERE ps_partkey IN ((SELECT pkey FROM q9_test_keys), (SELECT pkey FROM q9_test_keys) + 1);
UPDATE supplier SET s_suppkey = s_suppkey + 10, s_nationkey = CASE WHEN s_nationkey = (SELECT nkey FROM q9_test_keys) THEN s_nationkey + 10 ELSE s_nationkey END WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
UPDATE nation SET n_nationkey = n_nationkey + 10, n_name = n_name || 'K' WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
UPDATE orders SET o_orderkey = o_orderkey + 10 WHERE o_orderkey = (SELECT okey FROM q9_test_keys);
UPDATE lineitem SET l_partkey = CASE WHEN l_partkey = (SELECT pkey FROM q9_test_keys) THEN l_partkey + 10 ELSE l_partkey END, l_suppkey = l_suppkey + 10, l_orderkey = CASE WHEN l_orderkey = (SELECT okey FROM q9_test_keys) THEN l_orderkey + 10 ELSE l_orderkey END, l_extendedprice = l_extendedprice + 0.01 WHERE l_suppkey = (SELECT skey FROM q9_test_keys);
