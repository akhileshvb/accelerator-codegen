UPDATE orders SET o_orderpriority =
  CASE WHEN o_orderpriority = '2-HIGH' THEN '5-LOW' ELSE '1-URGENT' END
WHERE o_comment = 'q12_batch';
UPDATE lineitem SET l_shipmode = 'TRUCK'
WHERE l_comment = 'q12_batch_line' AND l_linenumber = 4;
