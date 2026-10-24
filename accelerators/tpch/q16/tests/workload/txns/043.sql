UPDATE partsupp SET ps_comment = 'v16 rekey part'
WHERE ps_partkey = (SELECT p_partkey FROM part WHERE p_name = 'Verus catalog 09');
UPDATE part SET p_partkey = (SELECT max(p_partkey) FROM part) + 100 WHERE p_name = 'Verus catalog 09';
UPDATE partsupp SET ps_comment = 'v16 rekey supplier'
WHERE ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-07');
UPDATE supplier SET s_suppkey = (SELECT min(s_suppkey) FROM supplier) - 100
WHERE s_name = 'Supplier#V16-07';
UPDATE partsupp SET ps_suppkey = (SELECT min(s_suppkey) FROM supplier) - 100
WHERE ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-05');
UPDATE supplier SET s_suppkey = (SELECT min(s_suppkey) FROM supplier) - 100
WHERE s_name = 'Supplier#V16-05';
