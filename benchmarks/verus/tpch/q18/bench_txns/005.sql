INSERT INTO lineitem
SELECT template.* REPLACE (8 AS l_linenumber, 25.00 AS l_quantity)
FROM lineitem AS template
WHERE l_comment = 'q18:line:01' AND l_linenumber = 1;
