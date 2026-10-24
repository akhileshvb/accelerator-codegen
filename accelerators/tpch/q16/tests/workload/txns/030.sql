UPDATE partsupp SET ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-07')
WHERE ps_partkey = (SELECT p_partkey FROM part WHERE p_name = 'Verus catalog 11');
