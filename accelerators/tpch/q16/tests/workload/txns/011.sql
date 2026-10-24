UPDATE partsupp
SET ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-08')
WHERE ps_partkey = (SELECT p_partkey FROM part WHERE p_name = 'Verus catalog 02')
  AND ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_name = 'Supplier#V16-02');
