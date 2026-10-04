INSERT INTO orders
SELECT template.* REPLACE (
  CAST(base.new_key + batch.number AS INTEGER) AS o_orderkey,
  CASE WHEN batch.number % 2 = 0 THEN '2-HIGH' ELSE '4-NOT SPECIFIED' END AS o_orderpriority,
  DATE '1995-08-01' AS o_orderdate, 'q12_batch' AS o_comment)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) template
CROSS JOIN (SELECT greatest((SELECT max(o_orderkey) FROM orders), (SELECT max(l_orderkey) FROM lineitem)) AS new_key) base
CROSS JOIN range(1, 9) batch(number);
INSERT INTO lineitem
SELECT template.* REPLACE (
  batch.o_orderkey AS l_orderkey, 1 AS l_linenumber,
  DATE '1995-08-10' AS l_shipdate, DATE '1995-08-12' AS l_commitdate,
  DATE '1995-08-15' AS l_receiptdate, 'FOB' AS l_shipmode,
  'q12_batch_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_batch') batch;
