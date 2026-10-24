INSERT INTO orders
SELECT (SELECT max(o_orderkey) FROM orders) + CAST(row_number() OVER (ORDER BY s_suppkey) AS INTEGER),
       (SELECT min(c_custkey) FROM customer), 'O', 200.00, DATE '1995-05-01',
       '2-HIGH', 'Clerk#000000001', 0, 'q20-extra/' || s_suppkey
FROM supplier WHERE s_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
INSERT INTO lineitem
SELECT o_orderkey, ps_partkey, ps_suppkey, 1, 20.00, 200.00, 0.00, 0.00,
       'N', 'O', DATE '1995-05-10', DATE '1995-05-11', DATE '1995-05-12',
       'DELIVER IN PERSON', 'TRUCK', 'q20-extra'
FROM partsupp JOIN orders ON o_comment = 'q20-extra/' || ps_suppkey WHERE ps_comment = 'q20-base';
