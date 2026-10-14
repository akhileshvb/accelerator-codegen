DELETE FROM lineitem
WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q4:seed:13' LIMIT 1)
  AND l_commitdate < l_receiptdate;
INSERT INTO orders
SELECT * REPLACE ('q4:variant2' AS o_comment, DATE '1993-09-30' AS o_orderdate,
    '5-LOW' AS o_orderpriority)
FROM orders WHERE o_comment = 'q4:variant';
