UPDATE supplier SET s_phone = '11-222-333-5555', s_address = '16 Warehouse Road'
WHERE s_comment = 'q15-new-supplier';
UPDATE lineitem SET l_extendedprice = 140000.00, l_discount = 0.03
WHERE l_comment = 'q15-new-supplier-line';
