UPDATE part SET p_brand = 'Brand#43' WHERE p_name = 'q19-restock';
UPDATE lineitem SET l_quantity = 36.00, l_discount = 0.07 WHERE l_comment = 'q19-restock-line';
