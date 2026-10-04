INSERT INTO partsupp SELECT (SELECT part_base + 125 FROM q2_test_keys), (SELECT supp_base FROM q2_test_keys), 0, 20.00, 'Existing supplier quote';
INSERT INTO partsupp SELECT (SELECT part_base + 125 FROM q2_test_keys), (SELECT supp_base + 3 FROM q2_test_keys), 0, -100.00, 'Unjoined quote must not lower minimum';
