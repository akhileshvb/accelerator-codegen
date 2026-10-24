UPDATE orders SET o_custkey = (SELECT max(c_custkey) + 1 FROM customer)
WHERE o_custkey = (
  SELECT c_custkey FROM customer
  WHERE c_comment = 'q22-seed' AND c_phone = '17-555-0199' AND c_acctbal = 9800.00
  ORDER BY c_custkey LIMIT 1
);
DELETE FROM customer
WHERE rowid = (
  SELECT rowid FROM customer
  WHERE c_comment = 'q22-seed' AND c_phone = '17-555-0199' AND c_acctbal = 9800.00
  ORDER BY rowid LIMIT 1
);
