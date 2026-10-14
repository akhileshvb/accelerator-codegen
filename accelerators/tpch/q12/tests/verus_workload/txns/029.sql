INSERT INTO orders
SELECT * REPLACE (
  (SELECT greatest((SELECT max(o_orderkey) FROM orders), (SELECT max(l_orderkey) FROM lineitem)) + 1) AS o_orderkey,
  '1-URGENT' AS o_orderpriority, 'q12_boundary_order' AS o_comment)
FROM orders ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_boundary_order') AS l_orderkey,
  dates.number AS l_linenumber, dates.ship AS l_shipdate,
  dates.commit_date AS l_commitdate, dates.receipt AS l_receiptdate,
  CASE WHEN dates.number % 2 = 0 THEN 'SHIP' ELSE 'MAIL' END AS l_shipmode,
  'q12_boundary_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN (VALUES
  (1, DATE '1994-12-29', DATE '1994-12-30', DATE '1994-12-31'),
  (2, DATE '1994-12-30', DATE '1994-12-31', DATE '1995-01-01'),
  (3, DATE '1995-12-29', DATE '1995-12-30', DATE '1995-12-31'),
  (4, DATE '1995-12-30', DATE '1995-12-31', DATE '1996-01-01'),
  (5, DATE '1995-06-14', DATE '1995-06-14', DATE '1995-06-15'),
  (6, DATE '1995-06-13', DATE '1995-06-15', DATE '1995-06-15'),
  (7, DATE '1995-06-15', DATE '1995-06-14', DATE '1995-06-16'),
  (8, DATE '1995-06-13', DATE '1995-06-16', DATE '1995-06-15'),
  (9, DATE '1995-06-15', DATE '1995-06-15', DATE '1995-06-15'),
  (10, DATE '1996-02-27', DATE '1996-02-28', DATE '1996-02-29')
) dates(number, ship, commit_date, receipt);
