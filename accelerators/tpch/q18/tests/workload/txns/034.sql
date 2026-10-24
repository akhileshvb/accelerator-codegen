DELETE FROM customer WHERE c_comment = 'q18:duplicate:customer';
DELETE FROM customer WHERE rowid = (
  SELECT MAX(rowid) FROM customer WHERE c_comment = 'q18:customer:01'
);
DELETE FROM orders WHERE o_comment = 'q18:duplicate:order';
DELETE FROM orders WHERE rowid = (
  SELECT MAX(rowid) FROM orders WHERE o_comment = 'q18:order:01'
);
DELETE FROM lineitem WHERE rowid = (
  SELECT MAX(rowid) FROM lineitem WHERE l_comment = 'q18:line:01' AND l_linenumber = 1
);
