INSERT INTO supplier SELECT * FROM supplier WHERE s_comment = 'q15-edge-a';
INSERT INTO supplier
SELECT * REPLACE ('revenue twin ' AS s_name, 'Boundary Avenue ' AS s_address,
  '' AS s_phone, 'q15-edge-a-variant' AS s_comment)
FROM supplier WHERE s_comment = 'q15-edge-a' ORDER BY rowid LIMIT 1;
INSERT INTO lineitem SELECT * FROM lineitem WHERE l_comment = 'q15-edge-a-main';
