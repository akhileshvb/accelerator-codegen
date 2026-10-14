INSERT INTO orders SELECT * FROM orders WHERE o_comment = 'q18:order:01';
INSERT INTO orders
SELECT template.* REPLACE (
  o_totalprice + 0.01 AS o_totalprice,
  'q18:duplicate:order' AS o_comment
)
FROM (SELECT * FROM orders WHERE o_comment = 'q18:order:01' LIMIT 1) AS template;
