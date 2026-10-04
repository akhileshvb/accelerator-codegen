CREATE TABLE test_q15_suspended_supplier AS SELECT * FROM supplier
WHERE s_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1997-01-01');
DELETE FROM supplier WHERE s_suppkey IN (SELECT s_suppkey FROM test_q15_suspended_supplier);
