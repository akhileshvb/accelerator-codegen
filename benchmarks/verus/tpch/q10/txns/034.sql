DELETE FROM lineitem WHERE rowid = (
  SELECT max(rowid) FROM lineitem
  WHERE l_orderkey = (SELECT min(o_orderkey) FROM orders WHERE o_comment = 'Q10-01') AND l_linenumber = 1
);
DELETE FROM orders WHERE rowid = (SELECT max(rowid) FROM orders WHERE o_comment = 'Q10-01');
DELETE FROM customer WHERE rowid = (SELECT max(rowid) FROM customer WHERE c_name = 'Q10-01');
DELETE FROM nation WHERE rowid = (SELECT max(rowid) FROM nation WHERE n_name = 'Q10 Federation');
