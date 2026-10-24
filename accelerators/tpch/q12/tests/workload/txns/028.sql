UPDATE lineitem SET l_orderkey =
  (SELECT o_orderkey FROM orders WHERE o_comment = 'q12_edge_order')
WHERE l_comment = 'q12_edge_line';
UPDATE orders SET o_orderpriority = '5-LOW' WHERE o_comment = 'q12_edge_order';
