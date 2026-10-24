UPDATE lineitem SET l_extendedprice = l_extendedprice + 10.00
WHERE (DATE_TRUNC('quarter', l_shipdate)::DATE, l_suppkey)
  IN (SELECT quarter, l_suppkey FROM test_q15_winners);
