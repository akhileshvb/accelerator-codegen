INSERT INTO partsupp SELECT q9_test_partsupp.* REPLACE (k.pkey AS ps_partkey, k.skey AS ps_suppkey, 70.00 AS ps_supplycost) FROM q9_test_partsupp CROSS JOIN q9_test_keys k;
INSERT INTO lineitem SELECT * FROM q9_test_removed_pair_lines;
