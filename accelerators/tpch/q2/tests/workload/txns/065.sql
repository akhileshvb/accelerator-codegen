UPDATE part SET p_mfgr = 'Manufacturer#5'
WHERE p_partkey IN (SELECT p_partkey FROM q2_test_parts ORDER BY p_partkey LIMIT 25);
