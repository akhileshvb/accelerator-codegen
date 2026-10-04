INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT max(o_orderkey) + 1 FROM orders) AS o_orderkey,
    DATE '1996-04-01' AS o_orderdate, 'q19-order-a' AS o_comment
) FROM orders AS template ORDER BY o_orderkey LIMIT 1;
INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q19-order-a') AS l_orderkey,
    'q19-order-a-line' AS l_comment
) FROM lineitem AS template WHERE l_comment IN ('q19-line-01','q19-line-02') AND l_linenumber <= 3;
