INSERT INTO orders SELECT * FROM orders WHERE o_comment = 'q4:seed:13';
INSERT INTO orders
SELECT * REPLACE (DATE '1994-01-01' AS o_orderdate, '2-HIGH' AS o_orderpriority,
    'q4:variant' AS o_comment)
FROM orders WHERE o_comment = 'q4:seed:13' LIMIT 1;
INSERT INTO lineitem
SELECT * REPLACE ('q4:duplicate-line' AS l_comment)
FROM lineitem WHERE l_comment = 'q4:seed:13' AND l_commitdate < l_receiptdate;
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'q4:duplicate-line';
