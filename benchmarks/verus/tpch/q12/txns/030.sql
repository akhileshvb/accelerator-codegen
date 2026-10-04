UPDATE lineitem SET l_shipdate = l_receiptdate - 2, l_commitdate = l_receiptdate - 1
WHERE l_comment = 'q12_boundary_line' AND l_linenumber BETWEEN 5 AND 9;
UPDATE lineitem SET l_receiptdate = DATE '1995-12-31'
WHERE l_comment = 'q12_boundary_line' AND l_linenumber = 4;
INSERT INTO orders SELECT * REPLACE ('1-urgent' AS o_orderpriority)
FROM orders WHERE o_comment = 'q12_boundary_order';
INSERT INTO orders SELECT * REPLACE ('2-HIGH ' AS o_orderpriority)
FROM orders WHERE o_comment = 'q12_boundary_order' AND o_orderpriority = '1-URGENT';
INSERT INTO lineitem
SELECT template.* REPLACE (
  (SELECT min(o_orderkey) FROM orders WHERE o_comment = 'q12_boundary_order') AS l_orderkey,
  modes.number AS l_linenumber,
  DATE '1995-06-13' AS l_shipdate, DATE '1995-06-14' AS l_commitdate,
  DATE '1995-06-15' AS l_receiptdate, modes.mode AS l_shipmode,
  'q12_text_line' AS l_comment)
FROM (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1) template
CROSS JOIN (VALUES (11, 'mail'), (12, 'MAIL '), (13, ' MAIL'), (14, ''), (15, 'Ship')) modes(number, mode);
