UPDATE partsupp SET ps_supplycost = ps_supplycost + 2.00, ps_availqty = ps_availqty + 3
WHERE ps_comment IN ('q11-small-1', 'q11-small-2');
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'JAPAN')
WHERE s_comment = 'q11-small-supplier';
