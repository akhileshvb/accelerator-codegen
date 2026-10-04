INSERT INTO lineitem
SELECT template.* REPLACE (
  batch.o_orderkey AS l_orderkey, CAST(lines.number AS INTEGER) AS l_linenumber,
  DATE '1995-09-10' AS l_shipdate, DATE '1995-09-12' AS l_commitdate,
  DATE '1995-09-15' AS l_receiptdate,
  CASE lines.number WHEN 2 THEN 'AIR' WHEN 3 THEN 'SHIP' ELSE 'REG AIR' END AS l_shipmode,
  'q12_batch_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_batch') batch
CROSS JOIN range(2, 5) lines(number);
