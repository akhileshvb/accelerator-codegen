INSERT INTO lineitem
SELECT * REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q4:variant') AS l_orderkey,
    DATE '1993-09-30' AS l_commitdate, DATE '1993-10-01' AS l_receiptdate,
    'q4:restored-shared' AS l_comment
) FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT 1;
UPDATE orders SET o_orderdate = DATE '1993-10-01', o_orderpriority = '1-URGENT'
WHERE o_comment = 'q4:variant2';
