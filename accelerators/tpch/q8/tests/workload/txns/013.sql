INSERT INTO customer SELECT source.* REPLACE (
  (SELECT max(c_custkey) + 1 FROM customer) AS c_custkey,
  'q8-customer-c' AS c_comment
) FROM customer source WHERE c_comment = 'q8-customer-b';
INSERT INTO orders SELECT source.* REPLACE (
  (SELECT max(o_orderkey) + 1 FROM orders) AS o_orderkey,
  (SELECT c_custkey FROM customer WHERE c_comment = 'q8-customer-c') AS o_custkey,
  'q8-order-09' AS o_comment
) FROM orders source WHERE o_comment = 'q8-order-04';
INSERT INTO lineitem SELECT source.* REPLACE (
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q8-order-09') AS l_orderkey,
  'q8-line-09-' || CAST(l_linenumber AS VARCHAR) AS l_comment
) FROM lineitem source WHERE l_orderkey = (SELECT o_orderkey FROM orders WHERE o_comment = 'q8-order-04');
