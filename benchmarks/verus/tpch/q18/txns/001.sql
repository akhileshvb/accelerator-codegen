INSERT INTO customer
SELECT template.* REPLACE (
  (SELECT MAX(c_custkey) + 1 FROM customer) AS c_custkey,
  'Q18 New Customer' AS c_name,
  'q18:new:customer' AS c_comment
)
FROM (SELECT * FROM customer ORDER BY c_custkey LIMIT 1) AS template;
INSERT INTO orders
SELECT template.* REPLACE (
  (SELECT MAX(o_orderkey) + 1 FROM orders) AS o_orderkey,
  (SELECT c_custkey FROM customer WHERE c_comment = 'q18:new:customer') AS o_custkey,
  'O' AS o_orderstatus,
  275000.25 AS o_totalprice,
  DATE '1995-06-15' AS o_orderdate,
  'q18:new:order' AS o_comment
)
FROM (SELECT * FROM orders ORDER BY o_orderkey LIMIT 1) AS template;
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q18:new:order') AS l_orderkey,
  CAST(line.sequence AS INTEGER) AS l_linenumber,
  45.00 AS l_quantity,
  'q18:new:line' AS l_comment
)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN range(1, 8) AS line(sequence);
