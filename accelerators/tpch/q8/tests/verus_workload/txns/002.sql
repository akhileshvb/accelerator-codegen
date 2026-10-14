INSERT INTO lineitem SELECT source.* REPLACE (
  (SELECT max(o_orderkey) + 1 FROM orders) AS l_orderkey,
  2150.50 AS l_extendedprice, 'q8-line-08' AS l_comment
) FROM lineitem source WHERE l_comment = 'q8-line-02-2';
INSERT INTO orders SELECT source.* REPLACE (
  (SELECT max(o_orderkey) + 1 FROM orders) AS o_orderkey,
  DATE '1996-11-21' AS o_orderdate, 'q8-order-08' AS o_comment
) FROM orders source WHERE o_comment = 'q8-order-02';
