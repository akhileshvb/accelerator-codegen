UPDATE supplier SET s_name = 'Quarterly Supply Co', s_phone = '10-123-456-7890'
WHERE s_suppkey = (SELECT l_suppkey FROM test_q15_winners WHERE quarter = DATE '1996-01-01');
