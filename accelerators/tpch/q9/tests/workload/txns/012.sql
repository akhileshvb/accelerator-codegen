UPDATE nation SET n_name = 'Q9 TEST NATION' WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
