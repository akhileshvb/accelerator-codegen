UPDATE lineitem SET l_extendedprice = l_extendedprice + 2.37
WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_comment = 'q19 fixture');
