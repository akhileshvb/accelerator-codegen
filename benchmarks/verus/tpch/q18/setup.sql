INSERT INTO customer
SELECT template.* REPLACE (
  (SELECT MAX(c_custkey) FROM customer) + fixture.sequence AS c_custkey,
  'Q18 Customer' || fixture.suffix AS c_name,
  'q18:customer:' || fixture.suffix AS c_comment
)
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS template
CROSS JOIN (VALUES (1, '01'), (2, '02'), (3, '03'), (4, '04'),
                   (5, '05'), (6, '06'), (7, '07'), (8, '08')) AS fixture(sequence, suffix);

INSERT INTO orders
SELECT template.* REPLACE (
  (SELECT MAX(o_orderkey) FROM orders) + fixture.sequence AS o_orderkey,
  customer.c_custkey AS o_custkey,
  'O' AS o_orderstatus,
  CAST(180000 + fixture.sequence * 12345.67 AS DECIMAL(15,2)) AS o_totalprice,
  DATE '1995-03-01' + CAST(fixture.sequence AS INTEGER) AS o_orderdate,
  'q18:order:' || fixture.suffix AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template
CROSS JOIN (VALUES (1, '01'), (2, '02'), (3, '03'), (4, '04'),
                   (5, '05'), (6, '06'), (7, '07'), (8, '08')) AS fixture(sequence, suffix)
JOIN customer ON customer.c_comment = 'q18:customer:' || fixture.suffix;

INSERT INTO lineitem
SELECT template.* REPLACE (
  orders.o_orderkey AS l_orderkey,
  CAST(line.sequence AS INTEGER) AS l_linenumber,
  CAST(CASE WHEN line.sequence < 7 THEN CEIL(fixture.quantity / 7.0)
       ELSE fixture.quantity - 6 * CEIL(fixture.quantity / 7.0) END AS DECIMAL(15,2)) AS l_quantity,
  'q18:line:' || fixture.suffix AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN (VALUES ('01', 280), ('02', 299), ('03', 300), ('04', 301),
                   ('05', 310), ('06', 330), ('07', 349), ('08', 350)) AS fixture(suffix, quantity)
JOIN orders ON orders.o_comment = 'q18:order:' || fixture.suffix
CROSS JOIN range(1, 8) AS line(sequence);
