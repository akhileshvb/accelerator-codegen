UPDATE nation SET n_name = n_name || '-A' WHERE n_nationkey IN (SELECT DISTINCT n_nationkey FROM q2_test_winners WHERE p_size = 10 AND r_name = 'ASIA' ORDER BY n_nationkey LIMIT 2);
