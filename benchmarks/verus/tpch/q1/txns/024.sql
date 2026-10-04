DELETE FROM lineitem
WHERE l_orderkey IN (
    SELECT o_orderkey FROM orders WHERE o_comment IN ('q1-new-small', 'q1-new-bulk')
);

DELETE FROM orders WHERE o_comment IN ('q1-new-small', 'q1-new-bulk');
