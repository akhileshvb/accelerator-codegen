DELETE FROM part WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys) AND p_type = 'POLISHED BRASS';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_regionkey = (SELECT r_regionkey FROM region WHERE r_name = 'AFRICA' ORDER BY r_regionkey LIMIT 1) ORDER BY n_nationkey LIMIT 1) WHERE s_suppkey = (SELECT supp_base + 3 FROM q2_test_keys);
