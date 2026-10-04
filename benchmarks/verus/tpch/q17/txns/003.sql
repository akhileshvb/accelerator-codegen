UPDATE lineitem SET l_quantity = 6.00 WHERE l_partkey = (SELECT p_partkey FROM part WHERE p_name = 'q17-fixture-01' LIMIT 1) AND l_linenumber = 1;
