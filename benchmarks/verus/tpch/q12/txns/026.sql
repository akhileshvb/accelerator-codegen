DELETE FROM orders WHERE o_comment = 'q12_edge_order' AND o_orderpriority = '1-URGENT';
UPDATE orders SET o_orderpriority = '2-HIGH' WHERE o_comment = 'q12_edge_order';
DELETE FROM lineitem WHERE l_comment = 'q12_edge_line';
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_edge_order') AS l_orderkey,
  CAST(lines.number AS INTEGER) AS l_linenumber,
  DATE '1995-06-10' AS l_shipdate, DATE '1995-06-12' AS l_commitdate,
  DATE '1995-06-15' AS l_receiptdate, 'SHIP' AS l_shipmode,
  'q12_edge_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN range(1, 4) lines(number);
