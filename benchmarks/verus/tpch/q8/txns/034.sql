DELETE FROM part WHERE rowid = (SELECT max(rowid) FROM part WHERE p_comment = 'q8-part-a');
DELETE FROM supplier WHERE rowid = (SELECT max(rowid) FROM supplier WHERE s_comment = 'q8-supplier-a');
DELETE FROM orders WHERE rowid = (SELECT max(rowid) FROM orders WHERE o_comment = 'q8-order-01');
DELETE FROM customer WHERE rowid = (SELECT max(rowid) FROM customer WHERE c_comment = 'q8-customer-a');
DELETE FROM lineitem WHERE rowid = (SELECT max(rowid) FROM lineitem WHERE l_comment = 'q8-line-01-1');
DELETE FROM nation WHERE rowid = (SELECT max(rowid) FROM nation WHERE n_name = 'BRAZIL');
DELETE FROM region WHERE rowid = (SELECT max(rowid) FROM region WHERE r_name = 'AMERICA');
