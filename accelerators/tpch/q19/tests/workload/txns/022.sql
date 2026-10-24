DELETE FROM part WHERE p_name = 'q19-restock';
UPDATE lineitem SET l_extendedprice = l_extendedprice - 0.75 WHERE l_comment = 'q19-line-12';
