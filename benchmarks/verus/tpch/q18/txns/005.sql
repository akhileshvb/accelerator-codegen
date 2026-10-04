INSERT INTO lineitem
SELECT template.* REPLACE (8 AS l_linenumber, 25.00 AS l_quantity)
FROM (SELECT * FROM lineitem WHERE l_comment = 'q18:new:line'
      ORDER BY l_linenumber LIMIT 1) AS template;
