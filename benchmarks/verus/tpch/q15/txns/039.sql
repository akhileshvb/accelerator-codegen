UPDATE lineitem SET l_extendedprice = 9999999999999.99, l_discount = 0.00
WHERE l_comment = 'q15-edge-a-main';
INSERT INTO lineitem
SELECT * REPLACE ((SELECT MAX(l_orderkey) + 1 FROM lineitem) AS l_orderkey,
  -9999999999999.98 AS l_extendedprice, 'q15-edge-cancellation' AS l_comment)
FROM lineitem WHERE l_comment = 'q15-edge-a-main';
UPDATE supplier SET s_suppkey = -2147483648 WHERE s_comment IN ('q15-edge-a', 'q15-edge-a-variant');
UPDATE lineitem SET l_suppkey = -2147483648
WHERE l_comment IN ('q15-edge-a-main', 'q15-edge-before', 'q15-edge-cancellation');
