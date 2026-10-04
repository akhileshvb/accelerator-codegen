UPDATE orders SET o_orderkey =
  (SELECT greatest((SELECT max(o_orderkey) FROM orders), (SELECT max(l_orderkey) FROM lineitem)) + 1)
WHERE o_comment = 'q12_edge_order';
