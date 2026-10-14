UPDATE supplier SET s_name = 'Catalog Supplier B Revised', s_address = '52 Market Street',
s_phone = '49-555-666-7777', s_comment = 'Annual contact refresh'
WHERE s_suppkey = (SELECT supp_base + 1 FROM q2_test_keys);
