UPDATE lineitem SET l_extendedprice = l_extendedprice + 50.00, l_discount = 0.02
WHERE l_comment = 'q15-batch-line';
UPDATE orders SET o_totalprice = (SELECT SUM(l_extendedprice * (1 - l_discount))
  FROM lineitem WHERE l_comment = 'q15-batch-line') WHERE o_comment = 'q15-batch-order';
