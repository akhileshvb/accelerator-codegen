INSERT INTO supplier SELECT supp_base + 2, 'Catalog Supplier C', '31 Dock Street', -nation_base,
'49-333-444-5555', 9999.00, 'Supplier reactivated' FROM q2_test_keys;
INSERT INTO partsupp SELECT part_base + 125, supp_base + 2, 500, 8.00, 'Revised quote'
FROM q2_test_keys;
