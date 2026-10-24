UPDATE supplier SET s_comment = 'Express delivery approved', s_phone = '49-333-444-5556'
WHERE s_suppkey = (SELECT supp_base + 2 FROM q2_test_keys);
