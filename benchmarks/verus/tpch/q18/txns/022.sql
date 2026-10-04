INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT MAX(o_orderkey) FROM orders) + fixture.sequence AS l_orderkey,
  CAST(line.sequence AS INTEGER) AS l_linenumber,
  48.00 AS l_quantity,
  'q18:batch:' || fixture.suffix AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (VALUES (1, '05'), (2, '06'), (3, '07'), (4, '08')) AS fixture(sequence, suffix)
CROSS JOIN range(1, 8) AS line(sequence);
INSERT INTO orders
SELECT template.* REPLACE (
  (SELECT MAX(o_orderkey) FROM orders) + fixture.sequence AS o_orderkey,
  customer.c_custkey AS o_custkey,
  CAST(220000 + fixture.sequence * 12500.75 AS DECIMAL(15,2)) AS o_totalprice,
  DATE '1996-04-01' + CAST(fixture.sequence AS INTEGER) AS o_orderdate,
  'q18:batch:' || fixture.suffix AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN (VALUES (1, '05'), (2, '06'), (3, '07'), (4, '08')) AS fixture(sequence, suffix)
JOIN customer ON customer.c_comment = 'q18:customer:' || fixture.suffix;
