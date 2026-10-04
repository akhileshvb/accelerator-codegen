INSERT INTO lineitem
SELECT * REPLACE (
  (SELECT greatest((SELECT max(o_orderkey) FROM orders), (SELECT max(l_orderkey) FROM lineitem)) + 1) AS l_orderkey,
  1 AS l_linenumber, DATE '1995-07-10' AS l_shipdate,
  DATE '1995-07-12' AS l_commitdate, DATE '1995-07-15' AS l_receiptdate,
  'RAIL' AS l_shipmode, 'q12_new_b_line' AS l_comment)
FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;
INSERT INTO orders
SELECT * REPLACE (
  (SELECT l_orderkey FROM lineitem WHERE l_comment = 'q12_new_b_line') AS o_orderkey,
  '5-LOW' AS o_orderpriority, 'q12_new_b' AS o_comment)
FROM orders ORDER BY o_orderkey LIMIT 1;
