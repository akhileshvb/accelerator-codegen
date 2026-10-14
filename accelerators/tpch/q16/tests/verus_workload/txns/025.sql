DELETE FROM partsupp WHERE ps_partkey IN (SELECT p_partkey FROM part WHERE p_name = 'Verus catalog 06');
DELETE FROM part WHERE p_name = 'Verus catalog 06';
