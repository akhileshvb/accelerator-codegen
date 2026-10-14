UPDATE nation SET n_regionkey = (SELECT r_regionkey FROM region WHERE r_name = 'ASIA' ORDER BY r_regionkey LIMIT 1) WHERE n_nationkey = (SELECT nation_base FROM q2_test_keys);
