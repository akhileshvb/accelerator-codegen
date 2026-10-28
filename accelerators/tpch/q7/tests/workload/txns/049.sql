-- Keys beyond 2^31: copies of every row of customer, supplier, orders, lineitem under the key offsets 3e9, 2^40 and 2^62 (c_custkey, l_orderkey, l_partkey, l_suppkey, o_custkey, o_orderkey, s_suppkey), linked to each other by the shifted keys
INSERT INTO customer SELECT * REPLACE (c_custkey + 3000000000 AS c_custkey) FROM customer WHERE c_custkey < 2147483648;
INSERT INTO supplier SELECT * REPLACE (s_suppkey + 3000000000 AS s_suppkey) FROM supplier WHERE s_suppkey < 2147483648;
INSERT INTO orders SELECT * REPLACE (o_orderkey + 3000000000 AS o_orderkey, o_custkey + 3000000000 AS o_custkey) FROM orders WHERE o_orderkey < 2147483648;
INSERT INTO lineitem SELECT * REPLACE (l_orderkey + 3000000000 AS l_orderkey, l_partkey + 3000000000 AS l_partkey, l_suppkey + 3000000000 AS l_suppkey) FROM lineitem WHERE l_orderkey < 2147483648;
INSERT INTO customer SELECT * REPLACE (c_custkey + 1099511627776 AS c_custkey) FROM customer WHERE c_custkey < 2147483648;
INSERT INTO supplier SELECT * REPLACE (s_suppkey + 1099511627776 AS s_suppkey) FROM supplier WHERE s_suppkey < 2147483648;
INSERT INTO orders SELECT * REPLACE (o_orderkey + 1099511627776 AS o_orderkey, o_custkey + 1099511627776 AS o_custkey) FROM orders WHERE o_orderkey < 2147483648;
INSERT INTO lineitem SELECT * REPLACE (l_orderkey + 1099511627776 AS l_orderkey, l_partkey + 1099511627776 AS l_partkey, l_suppkey + 1099511627776 AS l_suppkey) FROM lineitem WHERE l_orderkey < 2147483648;
INSERT INTO customer SELECT * REPLACE (c_custkey + 4611686018427387904 AS c_custkey) FROM customer WHERE c_custkey < 2147483648;
INSERT INTO supplier SELECT * REPLACE (s_suppkey + 4611686018427387904 AS s_suppkey) FROM supplier WHERE s_suppkey < 2147483648;
INSERT INTO orders SELECT * REPLACE (o_orderkey + 4611686018427387904 AS o_orderkey, o_custkey + 4611686018427387904 AS o_custkey) FROM orders WHERE o_orderkey < 2147483648;
INSERT INTO lineitem SELECT * REPLACE (l_orderkey + 4611686018427387904 AS l_orderkey, l_partkey + 4611686018427387904 AS l_partkey, l_suppkey + 4611686018427387904 AS l_suppkey) FROM lineitem WHERE l_orderkey < 2147483648;
