UPDATE supplier SET s_acctbal = 9900.00 WHERE s_suppkey = (SELECT supp_base + 1 FROM q2_test_keys);
UPDATE partsupp SET ps_supplycost = 39.00 WHERE ps_suppkey = (SELECT supp_base + 1 FROM q2_test_keys) AND ps_partkey IN (SELECT p_partkey FROM q2_test_parts ORDER BY p_partkey LIMIT 12);
