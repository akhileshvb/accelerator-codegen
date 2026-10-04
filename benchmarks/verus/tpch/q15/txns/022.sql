UPDATE supplier SET s_address = 'Central billing office', s_phone = '12-345-678-9012'
WHERE s_suppkey IN (SELECT l_suppkey FROM test_q15_winners);
