UPDATE supplier SET s_nationkey = (SELECT -nation_base FROM q2_test_keys)
WHERE s_suppkey = (SELECT supp_base + 3 FROM q2_test_keys);
UPDATE partsupp SET ps_supplycost = 10.00
WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys)
AND ps_suppkey = (SELECT supp_base + 3 FROM q2_test_keys);
