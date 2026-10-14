INSERT INTO orders SELECT source.* REPLACE (
  (SELECT max(o_orderkey) + 1 FROM orders) AS o_orderkey,
  DATE '1995-06-20' AS o_orderdate, 'q8-order-07' AS o_comment
) FROM orders source WHERE o_comment = 'q8-order-01';
INSERT INTO lineitem SELECT source.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q8-order-07') AS l_orderkey,
  1750.25 AS l_extendedprice, 'q8-line-07' AS l_comment
) FROM lineitem source WHERE l_comment = 'q8-line-01-1';
