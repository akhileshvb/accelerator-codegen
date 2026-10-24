INSERT INTO orders
SELECT * REPLACE (
  (SELECT greatest((SELECT max(o_orderkey) FROM orders), (SELECT max(l_orderkey) FROM lineitem)) + 1) AS o_orderkey,
  '1-URGENT' AS o_orderpriority, DATE '1995-05-01' AS o_orderdate,
  'q12_new_a' AS o_comment)
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_new_a') AS l_orderkey,
  CAST(batch.number AS INTEGER) AS l_linenumber,
  DATE '1995-06-10' AS l_shipdate, DATE '1995-06-12' AS l_commitdate,
  DATE '1995-06-15' AS l_receiptdate,
  CASE WHEN batch.number = 2 THEN 'SHIP' ELSE 'MAIL' END AS l_shipmode,
  'q12_new_a_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN range(1, 4) batch(number);
