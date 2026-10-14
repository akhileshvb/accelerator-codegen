UPDATE lineitem SET l_extendedprice = l_extendedprice * 1.05 WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'q17-fixture-%' AND p_size <= 8) AND l_linenumber = 1;
