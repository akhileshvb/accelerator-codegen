UPDATE lineitem SET l_shipdate = DATE '1996-04-01', l_commitdate = DATE '1996-04-02',
  l_receiptdate = DATE '1996-04-04' WHERE l_comment = 'q15-point-line';
UPDATE orders SET o_orderstatus = 'F' WHERE o_comment = 'q15-point-order';
