UPDATE nation SET n_name = 'FRANCE' WHERE n_nationkey = (SELECT nation_base FROM q2_test_keys);
