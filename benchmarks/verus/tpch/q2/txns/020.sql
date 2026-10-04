INSERT INTO supplier SELECT supp_base + 2, 'Catalog Supplier C', '31 Dock Street', nation_base, '49-333-444-5555', 9700.00, 'New approved supplier' FROM q2_test_keys;
INSERT INTO partsupp SELECT (SELECT part_base + 125 FROM q2_test_keys), (SELECT supp_base + 2 FROM q2_test_keys), 500, 10.00, 'Introductory quote';
