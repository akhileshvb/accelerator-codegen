INSERT INTO part SELECT * REPLACE ('GREEN' AS p_name) FROM part WHERE p_partkey = (SELECT pkey FROM q9_test_keys);
INSERT INTO part SELECT * REPLACE ('evergreen' AS p_name) FROM part WHERE p_partkey = (SELECT pkey FROM q9_test_keys) AND p_name LIKE '%green%';
INSERT INTO orders SELECT * REPLACE (DATE '2000-02-29' AS o_orderdate) FROM orders WHERE o_orderkey = (SELECT okey FROM q9_test_keys);
INSERT INTO supplier SELECT * REPLACE ((SELECT n_nationkey FROM q9_test_nation) AS s_nationkey) FROM supplier WHERE s_suppkey = (SELECT skey FROM q9_test_keys);
INSERT INTO nation SELECT * REPLACE ('Q9 TEST NATION ' AS n_name) FROM nation WHERE n_nationkey = (SELECT nkey FROM q9_test_keys);
INSERT INTO partsupp SELECT * REPLACE (0.01 AS ps_supplycost) FROM partsupp WHERE ps_partkey = (SELECT pkey FROM q9_test_keys);
INSERT INTO lineitem SELECT * REPLACE (0.01 AS l_quantity, 0.01 AS l_extendedprice, 1.00 AS l_discount) FROM lineitem WHERE l_orderkey = (SELECT okey FROM q9_test_keys) AND l_partkey = (SELECT pkey FROM q9_test_keys);
