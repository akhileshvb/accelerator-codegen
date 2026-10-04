DELETE FROM lineitem WHERE l_partkey IN (SELECT p_partkey FROM part WHERE p_comment = 'q8-part-c');
DELETE FROM part WHERE p_comment = 'q8-part-c';
