INSERT INTO lineitem
SELECT source.* REPLACE (
    (SELECT o_orderkey FROM orders WHERE o_comment = 'q6-batch-order-3') AS l_orderkey,
    5 AS l_linenumber,
    18.00 AS l_quantity,
    9876.54 AS l_extendedprice,
    0.06 AS l_discount,
    DATE '1995-08-15' AS l_shipdate,
    'q6-added-line' AS l_comment
)
FROM lineitem AS source WHERE l_comment = 'q6-batch-order-3' ORDER BY l_linenumber LIMIT 1;
UPDATE orders SET o_totalprice = o_totalprice + 9876.54 WHERE o_comment = 'q6-batch-order-3';
