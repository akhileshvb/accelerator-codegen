UPDATE supplier SET s_nationkey = (SELECT nkey FROM q9_test_keys) WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
UPDATE nation SET n_name = 'Q9 SUPPLIER REGION' WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
