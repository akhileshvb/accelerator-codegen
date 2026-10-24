INSERT INTO orders
SELECT * REPLACE (
  (SELECT GREATEST(MAX(o_orderkey), (SELECT MAX(l_orderkey) FROM lineitem)) + 1 FROM orders) AS o_orderkey,
  DATE '1996-03-01' AS o_orderdate, 'O' AS o_orderstatus,
  12036.00 AS o_totalprice, 'q15-batch-order' AS o_comment)
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q15-batch-order') AS l_orderkey,
  (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-01-01') AS l_suppkey,
  CAST(batch.line_no AS INTEGER) AS l_linenumber, 1.00 AS l_quantity,
  CAST(1500 + batch.line_no AS DECIMAL(15,2)) AS l_extendedprice, 0.00 AS l_discount,
  DATE '1996-03-15' AS l_shipdate, DATE '1996-03-16' AS l_commitdate,
  DATE '1996-03-18' AS l_receiptdate, 'q15-batch-line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN range(1, 9) batch(line_no);
