INSERT INTO customer SELECT * FROM customer WHERE c_comment = 'q18:customer:01';
INSERT INTO customer
SELECT template.* REPLACE (
  variant.customer_name AS c_name,
  'q18:duplicate:customer' AS c_comment
)
FROM customer AS template
CROSS JOIN (VALUES ('q18 customer01'), ('Q18 Customer01 ')) AS variant(customer_name)
WHERE template.c_comment = 'q18:customer:01'
  AND template.rowid = (SELECT MIN(rowid) FROM customer WHERE c_comment = 'q18:customer:01');
