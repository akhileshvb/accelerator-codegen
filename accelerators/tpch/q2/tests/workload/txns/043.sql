UPDATE partsupp SET ps_supplycost = 30.00 WHERE ps_partkey = (SELECT part_base + 125 FROM q2_test_keys);
UPDATE supplier SET s_acctbal = 9000.00, s_name = 'TieSupplier' WHERE s_suppkey IN ((SELECT supp_base FROM q2_test_keys), (SELECT supp_base + 1 FROM q2_test_keys));
