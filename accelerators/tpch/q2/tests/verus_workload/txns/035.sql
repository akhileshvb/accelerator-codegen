UPDATE part SET p_mfgr = 'Manufacturer#4' WHERE p_partkey IN (SELECT p_partkey FROM q2_test_parts ORDER BY p_partkey LIMIT 5);
UPDATE supplier SET s_acctbal = 9400.00 WHERE s_suppkey = (SELECT supp_base FROM q2_test_keys);
UPDATE partsupp SET ps_supplycost = 100.00 WHERE ps_suppkey = (SELECT supp_base + 1 FROM q2_test_keys) AND ps_partkey IN (SELECT p_partkey FROM q2_test_parts ORDER BY p_partkey LIMIT 5);
