DELETE FROM orders WHERE o_comment IN ('q12_boundary_order', 'q12_edge_order', 'q12_extreme_order');
DELETE FROM lineitem WHERE l_comment IN ('q12_boundary_line', 'q12_text_line', 'q12_edge_line', 'q12_extreme_line');
