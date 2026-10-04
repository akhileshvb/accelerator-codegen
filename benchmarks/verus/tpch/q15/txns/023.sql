CREATE TABLE test_q15_rescheduled AS
SELECT * FROM lineitem WHERE l_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-04-01')
  AND l_shipdate >= DATE '1996-04-01' AND l_shipdate < DATE '1996-07-01';
UPDATE lineitem SET l_shipdate = DATE '1996-07-01', l_commitdate = DATE '1996-07-02',
  l_receiptdate = DATE '1996-07-04'
WHERE (l_orderkey, l_linenumber) IN (SELECT l_orderkey, l_linenumber FROM test_q15_rescheduled);
