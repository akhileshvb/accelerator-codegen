UPDATE lineitem SET l_suppkey = (SELECT s_suppkey FROM supplier WHERE s_comment = 'q15-edge-b')
WHERE l_comment IN ('q15-edge-b-main', 'q15-edge-end');
CREATE TABLE test_q15_orphan_supplier AS
SELECT * REPLACE ((SELECT MAX(s_suppkey) + 1 FROM supplier) AS s_suppkey,
  'Missing Supplier' AS s_name, 'q15-edge-orphan' AS s_comment)
FROM supplier ORDER BY s_suppkey LIMIT 1;
INSERT INTO lineitem
SELECT * REPLACE ((SELECT MAX(l_orderkey) + 1 FROM lineitem) AS l_orderkey,
  (SELECT s_suppkey FROM test_q15_orphan_supplier) AS l_suppkey,
  300.00 AS l_extendedprice, 0.00 AS l_discount, DATE '2000-01-15' AS l_shipdate,
  DATE '2000-01-16' AS l_commitdate, DATE '2000-01-18' AS l_receiptdate,
  'q15-edge-orphan-line' AS l_comment)
FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;
