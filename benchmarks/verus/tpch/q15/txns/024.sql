UPDATE lineitem SET l_shipdate = saved.l_shipdate, l_commitdate = saved.l_commitdate,
  l_receiptdate = saved.l_receiptdate
FROM test_q15_rescheduled saved
WHERE lineitem.l_orderkey = saved.l_orderkey AND lineitem.l_linenumber = saved.l_linenumber;
