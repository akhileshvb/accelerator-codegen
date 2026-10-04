INSERT INTO lineitem
SELECT template.* REPLACE ('q1-exact-duplicate' AS l_comment)
FROM (SELECT * FROM lineitem WHERE l_shipdate <= DATE '1998-08-03'
      ORDER BY l_shipdate, l_orderkey, l_linenumber LIMIT 1) AS template
CROSS JOIN range(2) AS copies(sequence);
