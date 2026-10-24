INSERT INTO orders
SELECT * REPLACE (
  (SELECT greatest((SELECT max(o_orderkey) FROM orders), (SELECT max(l_orderkey) FROM lineitem)) + 1) AS o_orderkey,
  '1-URGENT' AS o_orderpriority, 'q12_edge_order' AS o_comment)
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO orders SELECT * FROM orders WHERE o_comment = 'q12_edge_order';
INSERT INTO orders SELECT * REPLACE ('5-LOW' AS o_orderpriority)
FROM orders WHERE o_comment = 'q12_edge_order' LIMIT 1;
INSERT INTO lineitem
SELECT * REPLACE (
  (SELECT min(o_orderkey) FROM orders WHERE o_comment = 'q12_edge_order') AS l_orderkey,
  1 AS l_linenumber, DATE '1995-06-10' AS l_shipdate,
  DATE '1995-06-12' AS l_commitdate, DATE '1995-06-15' AS l_receiptdate,
  'MAIL' AS l_shipmode, 'q12_edge_line' AS l_comment)
FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'q12_edge_line';
