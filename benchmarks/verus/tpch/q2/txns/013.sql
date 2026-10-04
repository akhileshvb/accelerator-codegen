UPDATE partsupp SET ps_supplycost = 20.00 WHERE ps_suppkey = (SELECT supp_base FROM q2_test_keys) AND ps_partkey IN (SELECT p_partkey FROM q2_test_parts ORDER BY p_partkey LIMIT 12);
