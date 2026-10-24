UPDATE supplier SET s_name = saved.s_name, s_address = saved.s_address, s_phone = saved.s_phone
FROM test_q15_saved_suppliers saved
WHERE supplier.s_suppkey = saved.s_suppkey
  AND supplier.s_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-01-01');
