UPDATE orders SET o_orderpriority =
  CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN '5-LOW' ELSE '2-HIGH' END
WHERE o_orderkey = (SELECT l_orderkey FROM lineitem WHERE l_comment = 'q12_extra_line');
DELETE FROM lineitem WHERE l_comment = 'q12_extra_line';
