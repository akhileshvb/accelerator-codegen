UPDATE supplier SET s_address = '42 Market Street, Suite 2'
WHERE s_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-01-01');
