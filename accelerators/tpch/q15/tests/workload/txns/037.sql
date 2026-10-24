UPDATE lineitem SET l_extendedprice = 1.00, l_discount = 2.00
WHERE l_comment LIKE 'q15-edge-%'
  AND l_shipdate >= DATE '2000-01-01' AND l_shipdate < DATE '2000-04-01';
UPDATE lineitem SET l_discount = 1.00 WHERE l_comment = 'q15-edge-orphan-line';
