DELETE FROM lineitem
WHERE l_orderkey IN (
    SELECT o_orderkey FROM orders WHERE o_comment = 'q1-new-bulk'
    ORDER BY o_orderkey LIMIT 3
);

DELETE FROM orders
WHERE o_orderkey IN (
    SELECT o_orderkey FROM orders WHERE o_comment = 'q1-new-bulk'
    ORDER BY o_orderkey LIMIT 3
);
