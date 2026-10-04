UPDATE lineitem SET l_extendedprice = l_extendedprice + 100.00
WHERE rowid = (SELECT rowid FROM lineitem
  WHERE l_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-01-01')
    AND l_shipdate >= DATE '1996-01-01' AND l_shipdate < DATE '1996-04-01'
  ORDER BY l_orderkey, l_linenumber LIMIT 1);
