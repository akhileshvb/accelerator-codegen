UPDATE part SET p_mfgr = 'Manufacturer#3' WHERE p_partkey IN (SELECT DISTINCT p_partkey FROM q2_test_winners WHERE p_size = 10 AND r_name = 'AFRICA' ORDER BY p_partkey LIMIT 6);
