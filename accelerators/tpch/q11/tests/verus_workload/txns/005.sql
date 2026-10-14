INSERT INTO partsupp
SELECT (SELECT ps_partkey FROM partsupp WHERE ps_comment = 'q11-new-stock-1'),
       s_suppkey, 1750, 82.50, 'q11-new-stock-2'
FROM supplier WHERE s_comment = 'q11-depot-b';
