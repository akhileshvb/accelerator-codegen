INSERT INTO partsupp
SELECT (SELECT max(ps_partkey) + 1 FROM partsupp), s_suppkey, 900, 557.50, 'q11-last-restock'
FROM supplier WHERE s_comment = 'q11-depot-a';
UPDATE partsupp SET ps_supplycost = ps_supplycost + 1.00 WHERE ps_comment = 'q11-stock-a1';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'INDIA')
WHERE s_comment = 'q11-depot-b';
