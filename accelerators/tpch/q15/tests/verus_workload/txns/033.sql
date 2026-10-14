DELETE FROM lineitem WHERE rowid = (SELECT rowid FROM lineitem
  WHERE l_comment = 'q15-edge-a-main' ORDER BY rowid LIMIT 1);
DELETE FROM supplier WHERE rowid = (SELECT rowid FROM supplier
  WHERE s_comment = 'q15-edge-a' ORDER BY rowid LIMIT 1);
UPDATE supplier SET s_name = 'REVENUE TWIN', s_address = ' Boundary Avenue', s_phone = ' '
WHERE s_comment = 'q15-edge-a-variant';
