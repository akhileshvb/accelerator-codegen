UPDATE part SET p_type = 'BRASS' WHERE p_partkey = (SELECT part_base + 125 FROM q2_test_keys);
UPDATE nation SET n_nationkey = -n_nationkey WHERE n_nationkey = (SELECT nation_base FROM q2_test_keys);
UPDATE supplier SET s_nationkey = -s_nationkey WHERE s_nationkey = (SELECT nation_base FROM q2_test_keys);
