UPDATE lineitem SET l_quantity = 50.00 WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'q17-fixture-%' AND p_size <= 16) AND l_linenumber = 5;
