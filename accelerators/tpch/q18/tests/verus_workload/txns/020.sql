INSERT INTO lineitem
SELECT template.* REPLACE (7 AS l_linenumber, 50.00 AS l_quantity)
FROM lineitem AS template
WHERE l_comment LIKE 'q18:batch:%' AND l_linenumber = 1;
