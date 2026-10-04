INSERT INTO orders
SELECT template.* REPLACE (extremes.order_key AS o_orderkey,
  CASE WHEN extremes.order_key < 0 THEN '1-URGENT' ELSE '5-LOW' END AS o_orderpriority,
  'q12_extreme_order' AS o_comment)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) template
CROSS JOIN (VALUES (-2147483648), (2147483647)) extremes(order_key);
INSERT INTO lineitem
SELECT template.* REPLACE (extremes.o_orderkey AS l_orderkey,
  1 AS l_linenumber,
  CASE WHEN extremes.o_orderkey < 0 THEN DATE '0001-06-13' ELSE DATE '9999-06-13' END AS l_shipdate,
  CASE WHEN extremes.o_orderkey < 0 THEN DATE '0001-06-14' ELSE DATE '9999-06-14' END AS l_commitdate,
  CASE WHEN extremes.o_orderkey < 0 THEN DATE '0001-06-15' ELSE DATE '9999-06-15' END AS l_receiptdate,
  'MAIL' AS l_shipmode, 'q12_extreme_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_extreme_order') extremes;
