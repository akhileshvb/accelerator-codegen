DELETE FROM partsupp
WHERE ps_comment LIKE 'v16%'
   OR ps_partkey IN (SELECT p_partkey FROM part WHERE p_name LIKE 'Verus %')
   OR ps_suppkey IN (SELECT s_suppkey FROM supplier WHERE s_name LIKE 'Supplier#V16-%');
DELETE FROM part WHERE p_name LIKE 'Verus %';
DELETE FROM supplier WHERE s_name LIKE 'Supplier#V16-%';
