UPDATE nation SET n_name = (SELECT n_name FROM q9_test_nation) WHERE n_nationkey = (SELECT nkey FROM q9_test_keys) + 10;
