INSERT INTO lineitem
SELECT template.* REPLACE (
    (SELECT max(o_orderkey) + 1 FROM orders) AS l_orderkey,
    'q19-order-b-line' AS l_comment
) FROM lineitem AS template WHERE l_comment IN ('q19-line-05','q19-line-06','q19-line-07') AND l_linenumber <= 3;
INSERT INTO orders
SELECT template.* REPLACE (
    (SELECT max(o_orderkey) + 1 FROM orders) AS o_orderkey,
    DATE '1996-04-02' AS o_orderdate, 'q19-order-b' AS o_comment
) FROM orders AS template ORDER BY o_orderkey LIMIT 1;
