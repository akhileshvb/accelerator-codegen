UPDATE lineitem SET l_discount = 0.04
WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_comment = 'q19 fixture');
