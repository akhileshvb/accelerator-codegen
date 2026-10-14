INSERT INTO lineitem
SELECT o_orderkey, ps_partkey, ps_suppkey, 3, 60.00, 100.00, 0.00, 0.00,
       'N', 'F', DATE '1995-12-31', DATE '1995-12-31', DATE '1995-12-31' + 2,
       'DELIVER IN PERSON', 'AIR', 'q20-base-C'
FROM partsupp JOIN orders ON o_comment = 'q20-order/' || ps_suppkey
WHERE ps_comment = 'q20-base' AND ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
