UPDATE orders SET o_totalprice = 123456.78, o_orderdate = DATE '1995-07-01'
WHERE o_comment IN ('q18:order:04', 'q18:order:08');
UPDATE customer SET c_name = '' WHERE c_comment = 'q18:customer:04';
UPDATE customer SET c_name = 'Q18 É' WHERE c_comment = 'q18:customer:08';
INSERT INTO customer
SELECT template.* REPLACE (
  variant.customer_name AS c_name,
  'q18:unicode:customer' AS c_comment
)
FROM customer AS template
CROSS JOIN (VALUES ('Q18 é'), ('Q18 É '), ('Q18 É')) AS variant(customer_name)
WHERE template.c_comment = 'q18:customer:08';
