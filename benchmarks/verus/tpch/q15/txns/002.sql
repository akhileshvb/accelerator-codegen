UPDATE lineitem SET l_discount = l_discount + 0.01
WHERE l_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-01-01')
  AND l_shipdate >= DATE '1996-01-01' AND l_shipdate < DATE '1996-04-01';
