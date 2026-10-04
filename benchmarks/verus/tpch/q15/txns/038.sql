DELETE FROM lineitem WHERE l_comment = 'q15-edge-orphan-line';
UPDATE lineitem SET l_extendedprice = -0.01, l_discount = 0.01
WHERE l_comment = 'q15-edge-b-main';
DELETE FROM lineitem WHERE l_comment = 'q15-edge-zero';
