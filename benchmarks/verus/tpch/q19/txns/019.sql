INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT p_partkey FROM part WHERE p_name = 'q19-restock') AS l_partkey,
    35.00 AS l_quantity, 4387.95 AS l_extendedprice, 'q19-restock-line' AS l_comment
) FROM lineitem AS template WHERE l_comment = 'q19-line-12' AND l_linenumber <= 3;
