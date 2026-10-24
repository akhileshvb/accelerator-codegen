UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'GERMANY')
WHERE s_comment = 'q11-new-depot';
UPDATE partsupp SET ps_supplycost = ps_supplycost + 0.15
WHERE ps_comment = 'q11-new-depot-stock';
