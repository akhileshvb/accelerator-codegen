UPDATE customer
SET c_custkey = (
  SELECT accounts.c_custkey FROM customer AS accounts
  WHERE accounts.c_comment NOT LIKE 'q22-%'
    AND EXISTS (SELECT 1 FROM orders WHERE o_custkey = accounts.c_custkey)
  ORDER BY accounts.c_custkey LIMIT 1
)
WHERE c_comment = 'q22-seed' AND c_phone = '17-555-0199' AND c_acctbal = 9800.00;
