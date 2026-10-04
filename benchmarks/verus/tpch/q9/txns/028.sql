INSERT INTO supplier SELECT q9_test_supplier.* REPLACE (k.skey AS s_suppkey, k.nkey AS s_nationkey) FROM q9_test_supplier CROSS JOIN q9_test_keys k;
INSERT INTO partsupp SELECT * FROM q9_test_removed_supplier_pairs;
INSERT INTO lineitem SELECT * FROM q9_test_removed_supplier_lines;
