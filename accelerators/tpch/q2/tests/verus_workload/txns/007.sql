UPDATE supplier SET s_address = '22 Harbor Road', s_phone = '49-222-333-4444', s_comment = 'Updated shipping contact' WHERE s_suppkey = (SELECT supp_base + 1 FROM q2_test_keys);
