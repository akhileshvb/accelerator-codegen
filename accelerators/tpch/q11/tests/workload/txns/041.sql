-- Keys beyond 2^31: copies of every row of supplier, partsupp under the key offsets 3e9, 2^40 and 2^62 (ps_partkey, ps_suppkey, s_suppkey), linked to each other by the shifted keys
INSERT INTO supplier SELECT * REPLACE (s_suppkey + 3000000000 AS s_suppkey) FROM supplier WHERE s_suppkey < 2147483648;
INSERT INTO partsupp SELECT * REPLACE (ps_partkey + 3000000000 AS ps_partkey, ps_suppkey + 3000000000 AS ps_suppkey) FROM partsupp WHERE ps_partkey < 2147483648;
INSERT INTO supplier SELECT * REPLACE (s_suppkey + 1099511627776 AS s_suppkey) FROM supplier WHERE s_suppkey < 2147483648;
INSERT INTO partsupp SELECT * REPLACE (ps_partkey + 1099511627776 AS ps_partkey, ps_suppkey + 1099511627776 AS ps_suppkey) FROM partsupp WHERE ps_partkey < 2147483648;
INSERT INTO supplier SELECT * REPLACE (s_suppkey + 4611686018427387904 AS s_suppkey) FROM supplier WHERE s_suppkey < 2147483648;
INSERT INTO partsupp SELECT * REPLACE (ps_partkey + 4611686018427387904 AS ps_partkey, ps_suppkey + 4611686018427387904 AS ps_suppkey) FROM partsupp WHERE ps_partkey < 2147483648;
