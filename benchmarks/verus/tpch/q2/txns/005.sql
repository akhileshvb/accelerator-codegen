UPDATE supplier SET s_acctbal = 9800.00 WHERE s_suppkey = (SELECT supp_base FROM q2_test_keys);
