UPDATE partsupp SET ps_availqty = 21 WHERE ps_comment = 'q20-base';
UPDATE lineitem SET l_quantity = 16.00 WHERE l_comment = 'q20-base-C';
INSERT INTO lineitem
SELECT o_orderkey, ps_partkey, ps_suppkey, 2, 14.00, 100.00, 0.00, 0.00,
       'N', 'F', DATE '1995-06-15', DATE '1995-06-15', DATE '1995-06-15' + 2,
       'DELIVER IN PERSON', 'AIR', 'q20-base-B'
FROM partsupp JOIN orders ON o_comment = 'q20-order/' || ps_suppkey
WHERE ps_comment = 'q20-base' AND ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %');
