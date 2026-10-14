UPDATE lineitem SET l_quantity = -9999999999999.99 WHERE l_comment = 'q20-base-B' AND l_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %' ORDER BY s_suppkey LIMIT 1);
INSERT INTO lineitem
SELECT o_orderkey, ps_partkey, ps_suppkey, 1, 0.00, 100.00, 0.00, 0.00,
       'N', 'F', DATE '1995-01-01', DATE '1995-01-01', DATE '1995-01-01' + 2,
       'DELIVER IN PERSON', 'AIR', 'q20-base-A'
FROM partsupp JOIN orders ON o_comment = 'q20-order/' || ps_suppkey
WHERE ps_comment = 'q20-base' AND ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_comment = 'q20-base' AND s_name LIKE 'Q20 CANADA %' ORDER BY s_suppkey DESC LIMIT 1);
