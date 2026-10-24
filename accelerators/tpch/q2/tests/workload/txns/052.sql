INSERT INTO supplier SELECT supp_base + 3, 'New Join Supplier', '44 Join Road', nation_base + 1, '49-444-555-6666', 9950.00, 'Delayed onboarding' FROM q2_test_keys;
INSERT INTO nation SELECT nation_base + 1, 'ÖSTERREICH', region_base + 1, 'New market' FROM q2_test_keys;
INSERT INTO region SELECT region_base + 1, 'EUROPE', 'New market region' FROM q2_test_keys;
