UPDATE customer SET c_comment = 'q22-reactivate'
WHERE c_custkey = (
  SELECT c_custkey FROM customer
  WHERE substring(c_phone, 1, 2) = '31'
    AND c_comment NOT LIKE 'q22-%'
    AND EXISTS (SELECT 1 FROM orders WHERE o_custkey = c_custkey)
  ORDER BY c_acctbal DESC, c_custkey LIMIT 1
);
DELETE FROM orders
WHERE o_custkey = (SELECT c_custkey FROM customer WHERE c_comment = 'q22-reactivate');
