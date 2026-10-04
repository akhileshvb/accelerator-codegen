DELETE FROM lineitem WHERE rowid=(SELECT max(rowid) FROM lineitem WHERE l_comment='q7 live 1');

DELETE FROM orders WHERE rowid=(SELECT max(rowid) FROM orders WHERE o_comment='q7 live');

DELETE FROM supplier WHERE rowid=(SELECT max(rowid) FROM supplier WHERE s_comment='q7 supplier A');

DELETE FROM customer WHERE rowid=(SELECT max(rowid) FROM customer WHERE c_comment='q7 customer A');

DELETE FROM nation WHERE rowid IN (SELECT max(rowid) FROM nation WHERE n_name IN ('FRANCE','GERMANY') GROUP BY n_name);
