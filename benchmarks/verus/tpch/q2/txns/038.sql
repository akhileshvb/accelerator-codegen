INSERT INTO partsupp SELECT (SELECT part_base + 125 FROM q2_test_keys), (SELECT supp_base FROM q2_test_keys), 150, 25.00, 'Restocked regular quote';
