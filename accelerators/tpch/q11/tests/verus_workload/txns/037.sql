UPDATE partsupp SET ps_partkey = (SELECT ps_partkey FROM partsupp WHERE ps_comment = 'q11-stock-a1')
WHERE ps_comment = 'q11-stock-a2';
UPDATE partsupp SET ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_comment = 'q11-depot-b')
WHERE ps_comment = 'q11-stock-a2';
UPDATE supplier SET s_suppkey = (SELECT max(s_suppkey) + 1 FROM supplier) WHERE s_comment = 'q11-depot-a';
UPDATE partsupp SET ps_suppkey = (SELECT s_suppkey FROM supplier WHERE s_comment = 'q11-depot-a')
WHERE ps_comment IN ('q11-stock-a1', 'q11-last-restock');
UPDATE nation SET n_nationkey = (SELECT max(n_nationkey) + 1 FROM nation) WHERE n_comment = 'q11-small-nation';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_comment = 'q11-small-nation')
WHERE s_comment = 'q11-small-supplier';
