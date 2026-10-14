CREATE TABLE test_q15_retired_supplier AS SELECT * FROM supplier
WHERE s_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1994-01-01');
CREATE TABLE test_q15_retired_lines AS SELECT * FROM lineitem
WHERE l_suppkey IN (SELECT s_suppkey FROM test_q15_retired_supplier);
DELETE FROM supplier WHERE s_suppkey IN (SELECT s_suppkey FROM test_q15_retired_supplier);
DELETE FROM lineitem WHERE l_suppkey IN (SELECT s_suppkey FROM test_q15_retired_supplier);
