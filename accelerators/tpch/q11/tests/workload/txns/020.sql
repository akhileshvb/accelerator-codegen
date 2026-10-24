UPDATE partsupp SET ps_availqty = ps_availqty + 50, ps_supplycost = ps_supplycost - 0.25
WHERE ps_comment = 'q11-restock-batch';
UPDATE supplier SET s_nationkey = (SELECT n_nationkey FROM nation WHERE n_name = 'CANADA')
WHERE s_comment = 'q11-depot-b';
