CREATE TABLE test_q15_cancelled AS
SELECT * FROM lineitem WHERE l_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1995-01-01')
  AND l_shipdate >= DATE '1995-01-01' AND l_shipdate < DATE '1995-04-01';
DELETE FROM lineitem WHERE (l_orderkey, l_linenumber)
  IN (SELECT l_orderkey, l_linenumber FROM test_q15_cancelled);
