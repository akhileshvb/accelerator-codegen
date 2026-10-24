INSERT INTO supplier SELECT * FROM q2_test_suppliers WHERE s_suppkey = (SELECT supp_base FROM q2_test_keys);
